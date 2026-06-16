# early_ioremap_reset() — ARM32 vs ARM64 Design Details

## 1. Is early_ioremap_reset() the Same on ARM32 and ARM64?

`early_ioremap_reset()` lives in `mm/early_ioremap.c` — **architecture-independent** code. Both ARM32 and ARM64 call this same function.

However, ARM32's `setup_arch()` calls it explicitly before `paging_init()`, while ARM64 has a different boot flow. The fixmap infrastructure itself differs between architectures.

---

## 2. ARM32 Fixmap Layout

```c
/* arch/arm/include/asm/fixmap.h */
enum fixed_addresses {
    FIX_EARLYCON_MEM_BASE,       /* early serial console */
    __end_of_permanent_fixed_addresses,
    FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
    FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,
    FIX_KMAP_BEGIN,              /* highmem kmap_atomic area */
    FIX_KMAP_END = FIX_KMAP_BEGIN + (KM_TYPE_NR * NR_CPUS) - 1,
    __end_of_fixed_addresses
};
```

ARM32 fixmap is near the top of kernel VA (below vectors page at 0xFFFF0000):
```
0xFFFE0000 – 0xFFFE1FFF  → FIX_KMAP area (kmap_atomic, highmem)
0xFFFD0000 – 0xFFFDFFFF  → FIX_BTMAP area (early_ioremap slots)
0xFFFC0000 – 0xFFFCFFFF  → FIX_EARLYCON_MEM_BASE
```

ARM32 fixmap is significantly smaller because kernel VA is precious (only 1GB total kernel VA).

---

## 3. ARM64 Fixmap Layout

```c
/* arch/arm64/include/asm/fixmap.h */
enum fixed_addresses {
    FIX_HOLE,
    FIX_FDT_END,
    FIX_FDT = FIX_FDT_END + FIX_FDT_SIZE / PAGE_SIZE - 1,
    FIX_EARLYCON_MEM_BASE,
    FIX_TEXT_POKE0,
    FIX_TEXT_POKE1,
    __end_of_permanent_fixed_addresses,
    FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
    FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,
    FIX_PTE,    FIX_PMD,   FIX_PUD,   FIX_PGD,  /* init_pg_dir fixup slots */
    __end_of_fixed_addresses
};
```

ARM64 fixmap also includes `FIX_FDT` — a large fixmap slot (2MB) for mapping the entire FDT blob. This is important: on ARM64, the FDT can be up to 2MB and must be mapped before the permanent page tables exist.

ARM64 fixmap is at the top of the upper kernel VA half:
```
0xFFFFFFFFFFFF0000 – 0xFFFFFFFFFFFFFFFF  → FIX_HOLE (safety gap)
0xFFFFFFFFFFFF1000 – ...                  → FIX_FDT (large)
...                                         → FIX_EARLYCON_MEM_BASE
...                                         → FIX_BTMAP area
```

ARM64 can afford larger fixmap slots because it has vast upper-half VA space.

---

## 4. The pte_offset_fixmap Mechanism: Common Infrastructure

Both ARM32 and ARM64 use the same function pointer mechanism from `mm/early_ioremap.c`:

```c
/* mm/early_ioremap.c — shared by all architectures */
pte_t * (*pte_offset_fixmap)(pmd_t *dir, unsigned long addr);

/* Before paging_init(): */
pte_offset_fixmap = pte_offset_early_fixmap;

/* After early_ioremap_reset(): */
pte_offset_fixmap = pte_offset_late_fixmap;
```

The architecture-specific parts:
- `pte_offset_early_fixmap`: **ARM32** uses a statically-allocated `pte_t[PTRS_PER_PTE]` array embedded in the `.init.data` section. **ARM64** similarly uses a pre-allocated PTE page.
- `pte_offset_late_fixmap`: Both architectures use the standard `pmd_page_vaddr(*pmd) + pte_index(addr)` page table walk.

---

## 5. ARM32 Early Fixmap Bootstrap

ARM32's early fixmap is set up by `early_ioremap_init()` (dir 07):

```c
/* arch/arm/mm/ioremap.c */
static pte_t bm_pte[PTRS_PER_PTE] __aligned(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE)
    __initdata;

static pte_t * __init pte_offset_early_fixmap(pmd_t *dir, unsigned long addr)
{
    return &bm_pte[pte_index(addr)];
}
```

`bm_pte` is a statically-allocated PTE table in `.init.data`. During `early_ioremap_init()`, the PMD entry for the fixmap VA region is populated to point to `bm_pte`. This allows fixmap to work without any memory allocation.

After `early_ioremap_reset()` and `paging_init()`:
- `prepare_page_table()` clears the PMD pointing to `bm_pte`
- `early_fixmap_shutdown()` (inside paging_init) allocates a new PTE page and connects it
- `pte_offset_late_fixmap` now uses this new PTE page

---

## 6. ARM64 Early Fixmap Bootstrap

ARM64 is slightly different because the FDT must be mapped VERY early (even before paging is properly initialized):

```c
/* arch/arm64/mm/mmu.c */
static pte_t bm_pte[PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss;

void __init early_fixmap_init(void)
{
    pgd_t *pgdp;
    p4d_t *p4dp;
    pud_t *pudp;
    pmd_t *pmdp;
    unsigned long addr = FIXADDR_START;

    pgdp = pgd_offset_k(addr);
    ...
    /* Connect bm_pud → bm_pmd → bm_pte to page table */
}
```

ARM64 pre-allocates full PUD, PMD, PTE tables in BSS. This allows fixmap mappings to work with the full 4-level page table hierarchy from very early on.

---

## 7. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| `early_ioremap_reset()` | Yes (explicit call in setup_arch) | Yes (called from paging_init flow) |
| Early fixmap PTE storage | `bm_pte[]` in `.init.data` | `bm_pte[]` in BSS |
| Full PUD/PMD pre-alloc | No (only PTE) | Yes (PUD, PMD, PTE pre-allocated) |
| FIX_FDT slot | No large FDT slot | Yes (2MB FDT fixmap slot) |
| kmap fixmap slots | Yes (FIX_KMAP — highmem kmap) | No (no highmem, no kmap needed) |
| `after_paging_init` guard | Yes (same mm/early_ioremap.c) | Yes (same code) |
| `pte_offset_late_fixmap` | Standard pmd walk | Same |
| Fixmap VA location | Top of 32-bit kernel VA | Top of upper half 64-bit VA |
| `TOTAL_FIX_BTMAPS` | Same definition | Same definition |

---

## 8. The FIX_FDT Fixmap on ARM64

ARM64 has a special large fixmap slot for the FDT that ARM32 lacks:

```c
/* ARM64 only */
#define FIX_FDT_SIZE    (MAX_FDT_SIZE + SZ_2M)
/* MAX_FDT_SIZE = 2MB */

FIX_FDT_END,
FIX_FDT = FIX_FDT_END + FIX_FDT_SIZE / PAGE_SIZE - 1,
```

This 2MB fixmap slot allows the kernel to map the full FDT into fixmap VA space using huge page (2MB) mappings. This is more efficient than mapping it page-by-page via FIX_BTMAP slots.

Usage in ARM64 boot:
```c
/* arch/arm64/kernel/setup.c */
void __init setup_arch(char **cmdline_p)
{
    /* FDT is already mapped via FIX_FDT slot from head.S */
    const void *fdt = get_early_fdt_ptr();
    ...
}
```

ARM32 does not have this because its FDT access pattern uses the standard FIX_BTMAP slots (fewer FDT bytes, accessed in chunks).

---

## 9. early_fixmap_shutdown() vs early_ioremap_reset()

These are two different functions, sometimes confused:

| Function | When | What |
|----------|------|------|
| `early_ioremap_reset()` | Before paging_init | Switches pte_offset_fixmap function pointer; sets after_paging_init=1 |
| `early_fixmap_shutdown()` | Inside paging_init | Called by paging_init AFTER new page tables exist; re-establishes fixmap with permanent PTEs |

ARM32 calls `early_ioremap_reset()` explicitly in `setup_arch()`. Then `paging_init()` internally calls `early_fixmap_shutdown()` to re-hook the fixmap to the new page tables.

ARM64's `paging_init()` → `map_kernel()` → `map_kernel_segment()` sets up fixmap in the permanent page tables.
