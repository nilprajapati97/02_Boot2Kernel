# early_mm_init() — Detailed Design: Bottom-to-Top Flow

## 1. Position in setup_arch() Boot Sequence

```
setup_arch()
  ├── parse_early_param()          ← sets vmalloc_size, mem= ranges
  └── [#ifdef CONFIG_MMU]
      └── early_mm_init(mdesc)     ← *** THIS FUNCTION *** (line 1147)
            ├── build_mem_type_table()
            └── early_paging_init(mdesc)  [ARM_PV_FIXUP / LPAE PV fixup]
```

`early_mm_init()` is **ARM32 MMU-only** (`#ifdef CONFIG_MMU`). It performs two preparatory tasks before the full `paging_init()`: (1) build the MMU memory type table that maps Linux abstract memory types to ARM hardware page table attribute bits, and (2) handle LPAE Physical-to-Virtual offset fixup for systems with >4GB physical RAM.

---

## 2. Source Code: `early_mm_init()`

**File:** `arch/arm/mm/mmu.c`

```c
void __init early_mm_init(const struct machine_desc *mdesc)
{
    build_mem_type_table();
    early_paging_init(mdesc);
}
```

Two calls. Both are heavy with detail — see sections below.

---

## 3. Call 1: `build_mem_type_table()`

**File:** `arch/arm/mm/mmu.c`

### 3.1 Purpose

`build_mem_type_table()` converts the abstract `mem_type[]` array entries into concrete hardware page-table descriptor bits specific to the detected CPU type. The result is cached in static arrays and used by every subsequent `create_mapping()` call in `paging_init()`.

### 3.2 Data Structures

```c
struct mem_type {
    pteval_t   prot_pte;        /* PTE attributes for small pages */
    pteval_t   prot_pte_s2;     /* Stage-2 PTE attributes (virtualization) */
    pmdval_t   prot_l1;         /* L1 (PMD) attributes */
    pmdval_t   prot_sect;       /* Section attributes */
    unsigned int domain;        /* ARM domain number (0-15) */
};

static struct mem_type mem_types[] = {
    [MT_DEVICE]          = { ... },
    [MT_DEVICE_NONSHARED]= { ... },
    [MT_DEVICE_CACHED]   = { ... },
    [MT_DEVICE_WC]       = { ... },
    [MT_UNCACHED]        = { ... },
    [MT_CACHECLEAN]      = { ... },
    [MT_MINICLEAN]       = { ... },
    [MT_LOW_VECTORS]     = { ... },
    [MT_HIGH_VECTORS]    = { ... },
    [MT_MEMORY_RWX]      = { ... },
    [MT_MEMORY_RW]       = { ... },
    [MT_MEMORY_RO]       = { ... },
    [MT_ROM]             = { ... },
    ...
};
```

### 3.3 What `build_mem_type_table()` Does

The function reads CPU type/capabilities and adjusts the `mem_types[]` entries:

```c
void __init build_mem_type_table(void)
{
    /* Read cache type from CP15 */
    unsigned int cachepolicy = ...;

    /* Adjust for write-allocate vs write-back-no-allocate */
    if (cpu_architecture() >= CPU_ARCH_ARMv6) {
        /* ARMv6+: use TEX[2:0] + C + B bits for cache type */
        for each mem_type: adjust prot_pte, prot_sect for TEX remap
    }

    /* Set domain bits */
    if (domain_supported) {
        mem_types[MT_MEMORY_RW].domain = DOMAIN_KERNEL;
        mem_types[MT_DEVICE].domain    = DOMAIN_IO;
        ...
    }

    /* LPAE: use 64-bit PTE descriptors */
    #ifdef CONFIG_ARM_LPAE
    for each mem_type: convert 32-bit attrs → 64-bit LPAE attrs
    #endif
}
```

**Key CPU-dependent decisions:**

| CPU Feature | Effect on mem_types |
|-------------|-------------------|
| ARMv5 and earlier | Uses `C+B` (2-bit) cache policy only |
| ARMv6+ | Uses `TEX[2:0]+C+B` (5-bit) cache policy |
| ARMv7 with VMSA | TEX remap disabled; explicit attr bits |
| LPAE (ARM Cortex-A7/A15) | 64-bit PTE format with `AttrIndx[2:0]` |
| Outer cache (L2) | Sets `TEX[2]` for outer cacheable |
| Write-allocate | Sets `TEX[1]` on normal memory |

### 3.4 ARM Hardware Page Table Attributes (ARM32 short-descriptor)

For a **section mapping** (1MB granularity, L1 descriptor):

```
 31                     20 19 18 17 16 15 14 12 11 10  9  8   5  4  3  2  1  0
┌──────────────────────────┬──┬──┬──┬──┬──┬───┬──┬──┬──┬──┬────┬──┬──┬──┬──┬──┐
│   Base Address [31:20]   │SS│ 0│nG│S │AP│TEX│AP│P │NS│ 0│Domain│XN│C │B │ 1│0 │
└──────────────────────────┴──┴──┴──┴──┴──┴───┴──┴──┴──┴──┴────┴──┴──┴──┴──┴──┘

AP[2:1] = Access Permission bits (read/write, user/kernel)
TEX[2:0] = Type Extension (cache policy)
C, B    = Cacheable, Bufferable (legacy)
XN      = Execute Never
S       = Shareable
nG      = Not Global (ASID-tagged)
Domain  = Domain number (0-15)
```

`build_mem_type_table()` fills in TEX+C+B+XN+S+AP for each `MT_*` type so that `create_mapping()` just reads `mem_types[map.type].prot_sect` and ORs it into the L1 descriptor.

### 3.5 LPAE (ARM Large Physical Address Extension) Descriptor Format

LPAE uses 64-bit descriptors. `build_mem_type_table()` converts:

```
 63   52 51     12 11 10  9  8  7  6  5  4  3  2  1  0
┌────────┬─────────┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
│ Upper  │ PA[51:12]│nG│AF│SH│AP│NS│MA│IA│XN│PXN│ Type │
└────────┴─────────┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘

AttrIndx[2:0] (MA) = index into MAIR_EL1 register (memory attribute table)
SH[1:0]       = Shareability
AP[2:1]       = Access Permission
AF            = Access Flag (set on first access)
```

---

## 4. Call 2: `early_paging_init(mdesc)` — LPAE PV Fixup

**File:** `arch/arm/mm/mmu.c`

### 4.1 What Problem Does This Solve?

ARM32 uses a linear mapping: `phys_addr = virt_addr - PAGE_OFFSET + PHYS_OFFSET`. This works when DRAM starts below 4GB. On systems like **Texas Instruments Keystone2** (66AK2H), physical RAM begins at 0x800000000 (32 GB). 

With LPAE enabled, ARM32 can **address up to 40-bit physical address space** but the virtual space is still 32-bit. The kernel binary was linked assuming `PHYS_OFFSET` = some low 32-bit value. `early_paging_init()` patches all the compiled-in Physical↔Virtual offset constants to account for the actual high physical address.

### 4.2 Call Flow

```c
static void __init early_paging_init(const struct machine_desc *mdesc)
{
    if (!mdesc->pv_fixup)
        return;           /* Most machines: no-op */

    offset = mdesc->pv_fixup();   /* Board code returns PA offset delta */
    if (offset == 0)
        return;

    /* Adjust kernel section physical addresses */
    kernel_sec_start += offset;
    kernel_sec_end   += offset;

    /* Get physical address of lpae_pgtables_remap_asm */
    lpae_pgtables_remap = (pgtables_remap *)__pa(lpae_pgtables_remap_asm);
    pa_pgd = __pa(swapper_pg_dir);

    /* Update __pv_offset and __pv_phys_pfn_offset */
    __pv_offset         += offset;
    __pv_phys_pfn_offset += PFN_DOWN(offset);

    /* Patch all __pa()/__va() call sites (patched binary constants) */
    fixup_pv_table(&__pv_table_begin,
        (&__pv_table_end - &__pv_table_begin) << 2);

    /* Flush caches, disable MMU temporarily (in assembly) */
    cr = get_cr();
    set_cr(cr & ~(CR_I | CR_C));   /* Disable I+D cache */
    flush_cache_all();

    /* Execute pgtable remap in physical address space (MMU off) */
    lpae_pgtables_remap(offset, pa_pgd);

    /* Re-enable caches */
    set_cr(cr);
}
```

### 4.3 Why Assembly? Why MMU Must Go Off?

The page tables themselves contain physical addresses. To remap the physical-to-virtual offset, the page tables must be rewritten. But modifying page tables while the MMU is using them can cause self-referential crashes — the TLB might be caching stale translations.

Solution: jump to identity-mapped assembly (`lpae_pgtables_remap_asm` in `arch/arm/mm/idmap.S`), disable MMU (`set_cr(cr & ~CR_M)`), rewrite page tables using purely physical addresses, re-enable MMU. The identity map makes this safe because virtual = physical during the MMU-off window.

### 4.4 Most Systems: This Is a No-Op

For the vast majority of ARM32 boards (Raspberry Pi, BeagleBone, Snapdragon 4xx with LPAE off, etc.), `mdesc->pv_fixup` is NULL. `early_paging_init()` returns immediately after the null check. Zero overhead.

---

## 5. Call Tree (Bottom-Up)

```
build_mem_type_table()          ← arch/arm/mm/mmu.c
  sets mem_types[] entries
  reads cachepolicy, CPU arch
  adjusts TEX+C+B or LPAE AttrIndx

early_paging_init(mdesc)        ← arch/arm/mm/mmu.c
  mdesc->pv_fixup()             ← board-specific (Keystone2 only)
  fixup_pv_table()              ← patches binary __pa/__va constants
  lpae_pgtables_remap()         ← arch/arm/mm/idmap.S (assembly)

early_mm_init(mdesc)            ← arch/arm/mm/mmu.c:1795
  called from setup_arch()      ← arch/arm/kernel/setup.c:1147
```

---

## 6. What Happens in Hardware

During `build_mem_type_table()`:
- CPU registers **are not written** — only software tables are updated.
- No TLB flush needed — page tables haven't been created yet for the new mappings.

During `early_paging_init()` (if PV fixup active):
- **CP15 C1 (SCTLR)**: `CR_I` and `CR_C` bits cleared → D-cache and I-cache disabled.
- **CP15 TTBR0**: The page table base register points to `swapper_pg_dir`. The assembly code rewrites the table entries at this physical address.
- **Full cache flush** (`flush_cache_all()`) ensures modified page table data is in RAM before MMU walks them.
- **TLB flush** (`local_flush_tlb_all()`) invalidates any cached translations.
- Then `CR_M` (MMU enable) is toggled off/on around the table rewrite.

---

## 7. Interview Q&A

**Q1: What does build_mem_type_table() actually build, and where is the result used?**
> It fills the global `mem_types[]` array with hardware-specific page-table attribute bits (TEX, C, B, AP, XN, Domain). `paging_init()` → `create_mapping()` reads `mem_types[map.type].prot_sect` or `.prot_pte` when writing page table descriptors. Without this initialization, `create_mapping()` would install page table entries with zero attributes — no caching, no access permissions — causing immediate faults.

**Q2: Why is build_mem_type_table() in early_mm_init() rather than in paging_init()?**
> `paging_init()` calls `create_mapping()` immediately. `create_mapping()` reads from `mem_types[]`. So `mem_types[]` must be populated before the first `create_mapping()` call. Separating the table build from the actual mapping is a clean design — you can validate the table in isolation, and it's called once regardless of how many mappings are created.

**Q3: What is the ARM PV (Physical-Virtual) fixup, and why is it only needed on LPAE?**
> In ARM32, `__pa(x) = x - PAGE_OFFSET + PHYS_OFFSET`. Both `PAGE_OFFSET` and `PHYS_OFFSET` are compile-time constants embedded throughout the kernel binary as immediate values in instruction encodings. On LPAE systems where physical RAM starts above 4GB (like Keystone2 at 0x800000000), the compile-time `PHYS_OFFSET` is wrong. The PV fixup patches all these embedded constants at boot, after the board code determines the true physical offset. This is not needed on non-LPAE ARM32 because physical addresses are always ≤ 4GB.

**Q4: What is `swapper_pg_dir`?**
> `swapper_pg_dir` is the kernel's master page directory — the level-1 page table used for the kernel virtual address space. On ARM32 without LPAE, it's a 4KB array of 4096 × 32-bit entries (each covering 1MB section). On LPAE, it's a 5-page structure (1 PGD + 4 PMD tables). It is defined in `arch/arm/kernel/head.S` and is the root of all kernel virtual address translations. `swapper_pg_dir` is loaded into TTBR1 (ARM32) or `TTBR1_EL1` (ARM64) to handle kernel-space address translation.

**Q5: How does the LPAE PV fixup avoid crashes when rewriting its own page tables?**
> The assembly function `lpae_pgtables_remap_asm` runs from the **identity map** — a special section where `physical address == virtual address`. When the MMU is disabled during the fixup, the CPU fetches instructions from physical addresses. Since the identity map makes VA=PA, the code continues executing correctly even with MMU off. After rewriting the page tables and re-enabling the MMU, the translations are now correct for the new physical offset.

**Q6: Is early_mm_init() present on ARM64?**
> No. ARM64 has `paging_init()` but no separate `early_mm_init()`. ARM64 uses a 4-level page table (PGD → PUD → PMD → PTE) with 64-bit descriptors defined by the ARMv8 architecture — no per-CPU-type table building is needed. The ARMv8 memory attributes are set via `MAIR_EL1` (Memory Attribute Indirection Register), which is a hardware register configured once, not via a software table.

**Q7: What is an ARM "domain"?**
> ARM domains are a hardware access control mechanism (ARMv7 and earlier) that divides memory into 16 groups. The Domain Access Control Register (DACR) can set each domain to `No Access`, `Client` (check page table permissions), or `Manager` (bypass permissions). The kernel uses domains to partition device memory (DOMAIN_IO), kernel memory (DOMAIN_KERNEL), and user memory (DOMAIN_USER). This allows fast switching of access permissions without TLB flushes (just write DACR). ARM64 dropped domains — it uses stage-2 page tables and explicit permission bits instead.
