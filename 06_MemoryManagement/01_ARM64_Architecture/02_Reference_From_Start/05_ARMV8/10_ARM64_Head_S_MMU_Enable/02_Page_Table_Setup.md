# ARM64 Page Table Setup Before MMU Enable

## 1. Overview

Before the MMU can be turned on, **two separate page tables** must exist in memory:

| Page Table | Symbol | Purpose |
|---|---|---|
| Identity map | `idmap_pg_dir` | PA == VA; survives the MMU-enable instant |
| Initial kernel map | `init_pg_dir` | Kernel image at high VA (`0xFFFF...`) |

Both are built in `head.S` before `__enable_mmu` is called.

---

## 2. Memory Layout of the Page Tables

```
Physical Memory (before MMU on):

  ┌─────────────────────────┐  ← _text (kernel load address, e.g. 0x4008_0000)
  │   Kernel Image          │
  │   (.text, .data, ...)   │
  ├─────────────────────────┤
  │   init_pg_dir           │  ← PAGE_SIZE * INIT_DIR_SIZE pages
  │   (initial page table)  │
  ├─────────────────────────┤
  │   idmap_pg_dir          │  ← PAGE_SIZE pages
  │   (identity page table) │
  └─────────────────────────┘
```

Both tables are statically allocated in the kernel binary (`__bss_start` area).

---

## 3. `create_idmap()` — Identity Map

**File:** `arch/arm64/kernel/head.S`

### Purpose
Maps the physical address of `__idmap_text_start` ... `__idmap_text_end`
so that `PA == VA`. This covers the `__enable_mmu` code itself.

### Why Needed
At the moment `SCTLR_EL1.M` is set, the Program Counter (PC) holds
a **physical address**. The CPU will immediately try to fetch the next
instruction through the MMU. If that PA is not mapped, Translation Fault.

The identity map ensures the physical page where `__enable_mmu` lives
is mapped at the same virtual address as its physical address.

### Code Flow

```asm
/*
 * Create the identity mapping
 * x0 = idmap_pg_dir, x1 = __idmap_text_start, x2 = __idmap_text_end
 */
SYM_FUNC_START_LOCAL(create_idmap)
    adrp    x0, idmap_pg_dir          // PGD base (physical)
    adrp    x3, __idmap_text_start    // start of idmap region
    adrp    x4, __idmap_text_end      // end of idmap region

    // Call map_memory to fill PGD/PUD/PMD/PTE entries
    // with PA == VA mappings for the idmap text region
    bl      __create_page_tables_helper
    ret
SYM_FUNC_END(create_idmap)
```

### What Gets Mapped

```
__idmap_text_start
    ├── cpu_resume_mmu
    ├── __enable_mmu         ← This is the critical function
    └── __idmap_text_end

PA range: [__idmap_text_start .. __idmap_text_end]
VA range: same (PA == VA)
```

---

## 4. `__create_page_tables()` — Kernel Page Table

**File:** `arch/arm64/kernel/head.S`

### Purpose
Maps the kernel image into the **high virtual address** range starting at
`KIMAGE_VADDR` (typically `0xFFFF800008000000` or similar, depending on config).

Also maps:
- The kernel text+data (`_text` ... `_end`)
- The FDT (Device Tree Blob) — needed early for `unflatten_device_tree`
- `init_pg_dir` itself (so it can be freed as normal memory later)

### Code Flow (simplified)

```asm
SYM_FUNC_START_LOCAL(__create_page_tables)
    adrp    x0, init_pg_dir           // output PGD base
    mov     x1, x0
    add     x2, x1, #INIT_DIR_SIZE    // end of init_pg_dir area
1:  stp     xzr, xzr, [x1], #16      // zero-fill entire table
    cmp     x1, x2
    b.lo    1b

    // Map kernel image: VA = KIMAGE_VADDR, PA = _text
    adrp    x5, _text
    adrp    x6, _end
    mov_q   x7, KIMAGE_VADDR
    bl      map_kernel_image

    // Map FDT
    bl      map_fdt

    ret
SYM_FUNC_END(__create_page_tables)
```

### Kernel VA Layout Built Here

```
KIMAGE_VADDR (e.g. 0xFFFF800008000000)
  ├── .text   (code, mapped RX)
  ├── .rodata (mapped R)
  ├── .data   (mapped RW)
  └── .bss    (mapped RW, zeroed)
```

---

## 5. `map_memory` Helper — Walking the Page Table

Both `create_idmap` and `__create_page_tables` use a shared helper
`map_memory` (in `head.S`) to populate page table entries.

### Page Table Walk (4-level, 4KB pages, 48-bit VA)

```
VA [47:39] → PGD index  (512 entries)
VA [38:30] → PUD index  (512 entries)
VA [29:21] → PMD index  (512 entries, or 2MB hugepage)
VA [20:12] → PTE index  (512 entries)
VA [11:0]  → page offset (4096 bytes)
```

### Assembly Pattern (simplified)

```asm
// x0 = PGD base
// x1 = VA start, x2 = VA end, x3 = PA start, x4 = flags

map_memory:
    lsr     x6, x1, #PGDIR_SHIFT    // PGD index
    and     x6, x6, #PTRS_PER_PGD-1
    ldr     x7, [x0, x6, lsl #3]   // load PGD entry
    cbz     x7, alloc_pud           // if empty, allocate PUD

    // walk to PUD, PMD, PTE...
    // set block or page descriptor with PA + flags
```

### Descriptor Flags Used

```
PTE_ATTRINDX(MT_NORMAL)    // normal memory (from MAIR_EL1 index)
PTE_AF                     // access flag (must be set or fault on access)
PTE_SHARED                 // inner shareable
PTE_UXN                    // unprivileged execute-never
PTE_DIRTY                  // dirty bit (on capable hardware)
```

---

## 6. `idmap_pg_dir` vs `init_pg_dir` vs `swapper_pg_dir`

| Symbol | When Active | Scope | Freed? |
|---|---|---|---|
| `idmap_pg_dir` | During `__enable_mmu` only | PA == VA identity map | No (kept for CPU hotplug, suspend/resume) |
| `init_pg_dir` | From MMU-on until `paging_init()` | Kernel image only | Yes — freed in `paging_init()` |
| `swapper_pg_dir` | After `paging_init()` (permanent) | Full kernel VA layout | Never |

---

## 7. Full Sequence Diagram

```
head.S
  │
  ├─ create_idmap()
  │     │
  │     └─ map_memory(idmap_pg_dir, PA_idmap_text, PA_idmap_text_end,
  │                   PA_idmap_text, flags)   ← PA == VA
  │
  ├─ __create_page_tables()
  │     │
  │     ├─ zero-fill init_pg_dir
  │     ├─ map_memory(init_pg_dir, KIMAGE_VADDR, KIMAGE_VADDR+kernel_size,
  │     │             _text_phys, flags)
  │     └─ map_memory(init_pg_dir, fdt_vaddr, fdt_vaddr+fdt_size,
  │                   fdt_phys, flags)
  │
  ├─ msr TTBR0_EL1, idmap_pg_dir
  ├─ msr TTBR1_EL1, init_pg_dir
  │
  └─ __enable_mmu()   ← MMU ON using these tables
```

---

## 8. Common Questions

### Q: Why does idmap_pg_dir use PA == VA?
Because the CPU PC is still a physical address when the MMU turns on.
Mapping `PA == VA` means the CPU can continue executing at the same address
after the MMU is on — the translation simply returns the same value.

### Q: Why is init_pg_dir temporary?
`init_pg_dir` only maps the kernel image statically (at compile time).
It does not map device memory, vmalloc space, modules, or fixmap.
`paging_init()` builds `swapper_pg_dir` which covers all of these.

### Q: What happens to init_pg_dir after paging_init()?
```c
// arch/arm64/mm/mmu.c
void __init paging_init(void)
{
    ...
    cpu_replace_ttbr1(lm_alias(swapper_pg_dir), init_pg_dir);
    init_mm.pgd = swapper_pg_dir;
    memblock_free(__pa_symbol(init_pg_dir), INIT_DIR_SIZE);  // freed
    ...
}
```
The pages are returned to `memblock` and eventually to the buddy allocator.
