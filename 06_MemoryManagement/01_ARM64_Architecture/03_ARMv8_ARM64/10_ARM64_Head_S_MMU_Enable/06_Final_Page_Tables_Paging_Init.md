# Final Page Tables — `paging_init()` and `swapper_pg_dir`

## 1. Why Temporary Tables Are Not Enough

When the MMU is first enabled (in `head.S`), the kernel uses `init_pg_dir` —
a **minimal, statically allocated** page table that only maps:
- The kernel image (`.text`, `.data`, `.bss`)
- The FDT (device tree blob)

`init_pg_dir` does **not** map:
- `vmalloc` / `ioremap` range
- `vmemmap` (struct page array)
- Modules region
- `fixmap` region
- Peripheral / device memory
- Memory beyond a simple linear map

`paging_init()` builds the **permanent** page tables that cover all of this.

---

## 2. Position in the Boot Flow

```
start_kernel()
    └── setup_arch(&command_line)         // arch/arm64/kernel/setup.c
            │
            ├── early_fixmap_init()
            ├── early_ioremap_init()
            ├── parse_early_param()
            ├── arm64_memblock_init()
            │
            └── paging_init()             ◄── builds swapper_pg_dir
                    │
                    ├── map_kernel()
                    ├── map_mem()
                    ├── pgd_clear_ed()
                    ├── cpu_replace_ttbr1(swapper_pg_dir)   ◄── switch!
                    ├── init_mm.pgd = swapper_pg_dir
                    └── memblock_free(init_pg_dir)          ◄── free old table
```

---

## 3. `swapper_pg_dir` vs `init_pg_dir`

| Feature | `init_pg_dir` | `swapper_pg_dir` |
|---|---|---|
| **When active** | head.S → paging_init | paging_init → forever |
| **Built in** | `head.S` (assembly) | `paging_init()` (C) |
| **Maps** | Kernel image + FDT only | Full kernel VA layout |
| **Page table size** | `INIT_DIR_SIZE` (few pages) | Dynamically allocated |
| **Freed** | Yes (by `paging_init`) | Never |
| **TTBR1** | Loaded at boot | Loaded by `cpu_replace_ttbr1()` |

---

## 4. `paging_init()` — Step by Step

**File:** `arch/arm64/mm/mmu.c`

```c
void __init paging_init(void)
{
    pgd_t *pgdp = pgd_set_fixmap(__pa_symbol(swapper_pg_dir));

    // Step 1: Map the kernel image (text, rodata, data, bss)
    map_kernel(pgdp);

    // Step 2: Map all physical memory as linear map
    // (PAGE_OFFSET + 0 .. PAGE_OFFSET + DRAM_SIZE)
    map_mem(pgdp);

    pgd_clear_fixmap();

    // Step 3: Install the new page table into TTBR1_EL1
    cpu_replace_ttbr1(lm_alias(swapper_pg_dir), init_pg_dir);

    // Step 4: Update kernel's mm_struct
    init_mm.pgd = swapper_pg_dir;

    // Step 5: Free the old temporary init_pg_dir
    memblock_free(__pa_symbol(init_pg_dir), INIT_DIR_SIZE);

    // Step 6: Set up vmemmap (struct page array)
    // (happens later in sparse_init)
}
```

---

## 5. `map_kernel()` — Kernel Image Mapping

**File:** `arch/arm64/mm/mmu.c`

```c
static void __init map_kernel(pgd_t *pgdp)
{
    // Map .text section: RX (read + execute)
    map_kernel_segment(pgdp, _text, _etext,
                       PAGE_KERNEL_EXEC, &vmlinux_text, ...);

    // Map .rodata section: R (read-only)
    map_kernel_segment(pgdp, __start_rodata, __inittext_begin,
                       PAGE_KERNEL_RO, ...);

    // Map .init.text section: RX
    map_kernel_segment(pgdp, __inittext_begin, __inittext_end,
                       PAGE_KERNEL_EXEC, ...);

    // Map .init.data + .data + .bss: RW
    map_kernel_segment(pgdp, __initdata_begin, _end,
                       PAGE_KERNEL, ...);

    // Map the page tables themselves (needed to modify them)
    ...
}
```

### Memory Protection by Section

```
.text    → PAGE_KERNEL_EXEC  = PTE_RDONLY | PTE_VALID | not UXN
.rodata  → PAGE_KERNEL_RO    = PTE_RDONLY | PTE_UXN | PTE_PXN
.data    → PAGE_KERNEL       = PTE_DIRTY  | PTE_UXN | PTE_PXN
.bss     → PAGE_KERNEL       = same as .data
```

W^X (Write XOR Execute) is enforced — no section is both writable and executable.

---

## 6. `map_mem()` — Linear Map of Physical Memory

**File:** `arch/arm64/mm/mmu.c`

The linear map makes all DRAM accessible via `PAGE_OFFSET + PA`:

```c
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t start, end;
    u64 i;

    // Walk memblock.memory regions
    for_each_mem_range(i, &start, &end) {

        // Skip the kernel image itself (already mapped by map_kernel)
        if (start < __pa_symbol(_end) && end > __pa_symbol(_text))
            continue;

        // Create linear map: VA = __phys_to_virt(PA), PA = PA
        __map_memblock(pgdp, start, end,
                       pgprot_tagged(PAGE_KERNEL),
                       NO_CONT_MAPPINGS);
    }
}
```

### Linear Map Layout

```
PA 0x4000_0000 (1GB)   →   VA 0xFFFF_0000_4000_0000   (PAGE_OFFSET + PA)
PA 0x4008_0000 (kernel) →  VA 0xFFFF_0000_4008_0000   (kernel's physical location)
PA 0x8000_0000 (2GB)   →   VA 0xFFFF_0000_8000_0000
```

The kernel image is accessible at **both**:
1. `KIMAGE_VADDR` (via `map_kernel` mapping — used for code execution)
2. `PAGE_OFFSET + PA` (via linear map — used for direct physical access)

---

## 7. `cpu_replace_ttbr1()` — The Switch

**File:** `arch/arm64/mm/mmu.c` / `proc.S`

This is the moment `swapper_pg_dir` replaces `init_pg_dir` in `TTBR1_EL1`:

```c
void cpu_replace_ttbr1(pgd_t *pgdp, pgd_t *old)
{
    typedef void (ttbr_replace_func)(phys_addr_t);
    extern ttbr_replace_func idmap_cpu_replace_ttbr1;
    ttbr_replace_func *replace_phys;

    // Run from the identity map (must be in idmap since we're changing TTBR1)
    replace_phys = (void *)__pa_symbol(idmap_cpu_replace_ttbr1);

    cpu_install_idmap();              // switch TTBR0 to idmap temporarily
    replace_phys(__pa(pgdp));         // write new TTBR1_EL1
    cpu_uninstall_idmap();            // restore TTBR0 for current task
}
```

### Why Must This Run from the Identity Map?

When writing `TTBR1_EL1`, the CPU briefly has no valid kernel translation.
Any instruction fetch during this window must be covered by the **identity map**
(`TTBR0`) to avoid a fault.

This is the same principle as the original `__enable_mmu` — we need the
identity map as a "safety net" when modifying the kernel's TTBR.

---

## 8. After `paging_init()` — Steady State

```
TTBR1_EL1  =  swapper_pg_dir  (permanent, never changes)
TTBR0_EL1  =  per-process pgd  (changes on every context switch)

swapper_pg_dir maps:
  ├── kernel .text          RX
  ├── kernel .rodata        R
  ├── kernel .data / .bss   RW
  ├── linear map            RW  (all DRAM at PAGE_OFFSET)
  ├── vmalloc region        dynamic (populated on demand by vmalloc/ioremap)
  ├── vmemmap               RW  (struct page array)
  ├── fixmap                RW  (compile-time fixed VAs)
  └── modules               RX  (loaded by module loader)
```

---

## 9. `init_pg_dir` Lifecycle Summary

```
head.S:  __create_page_tables() ──► allocates and fills init_pg_dir
head.S:  __enable_mmu()         ──► TTBR1 = init_pg_dir (MMU ON)
C code:  start_kernel()
         setup_arch()
         paging_init()          ──► builds swapper_pg_dir
                                ──► TTBR1 = swapper_pg_dir (SWITCHED)
                                ──► memblock_free(init_pg_dir) ──► FREED
```

The `init_pg_dir` pages are returned to `memblock` and later given to the
buddy allocator as free pages.

---

## 10. Verification — Checking the Current TTBR1

```c
// From kernel (EL1):
u64 ttbr1;
asm("mrs %0, ttbr1_el1" : "=r"(ttbr1));
pr_info("TTBR1_EL1 = 0x%016llx\n", ttbr1);

// Should match:
pr_info("swapper_pg_dir PA = 0x%016llx\n",
        (u64)__pa_symbol(swapper_pg_dir));
```
