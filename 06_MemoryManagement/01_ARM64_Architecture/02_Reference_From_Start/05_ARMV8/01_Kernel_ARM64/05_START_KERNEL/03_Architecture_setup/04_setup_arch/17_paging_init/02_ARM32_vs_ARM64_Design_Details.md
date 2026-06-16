# paging_init() — ARM32 vs ARM64 Design Details

## 1. Overview: Same Name, Different Architecture

Both ARM32 and ARM64 have a `paging_init()` function, but they differ significantly in:
- Page table levels (2-level ARM32, 3/4-level ARM64)
- Memory model (lowmem/highmem vs flat linear map)
- Huge page sizes (1MB sections vs 2MB PMD)
- KASLR (none on ARM32 vs address randomization on ARM64)

---

## 2. Page Table Levels

### ARM32 Page Table (2-level without LPAE)

```
CR3-like: TTBR0/TTBR1 (Translation Table Base Register)

Level 1: PGD (Page Global Directory)
  4096 entries × 4 bytes = 16KB
  Each entry covers 1MB of VA
  Entry types: section (1MB page) or table (points to L2)

Level 2: PTE (Page Table Entry)
  256 entries × 4 bytes = 1KB
  Each entry covers 4KB of VA
  Only used when L1 entry is table (not section)
```

ARM32 without LPAE uses 2-level page tables. 1MB "sections" allow efficient mapping of large regions.

### ARM32 Page Table (3-level with LPAE)

```
With CONFIG_ARM_LPAE (for >4GB physical address support):

Level 1: PGD (3 entries for kernel range)
Level 2: PMD (512 entries × 8 bytes = 4KB)
  Each entry covers 2MB
  Entry types: hugepage (2MB) or table (points to L3)
Level 3: PTE (512 entries × 8 bytes = 4KB)
  Each entry covers 4KB
```

LPAE brings ARM32 closer to ARM64's 3-level structure.

### ARM64 Page Table (4-level, 4KB pages, 48-bit VA)

```
Level 0: PGD (512 entries × 8 bytes = 4KB)
  Each entry covers 512GB of VA
Level 1: PUD (512 entries × 8 bytes = 4KB)
  Each entry covers 1GB of VA
  Can be hugepage (1GB) or table
Level 2: PMD (512 entries × 8 bytes = 4KB)
  Each entry covers 2MB of VA
  Can be hugepage (2MB) or table
Level 3: PTE (512 entries × 8 bytes = 4KB)
  Each entry covers 4KB of VA
```

ARM64 uses 4-level (or 3-level with 16KB pages, 36-bit VA) depending on configuration.

---

## 3. ARM32 paging_init() vs ARM64 paging_init()

### ARM32 paging_init() — arch/arm/mm/mmu.c

```c
void __init paging_init(const struct machine_desc *mdesc)
{
    void *zero_page;

    build_mem_type_table();      /* compute cacheable/non-cacheable attributes */
    prepare_page_table();        /* clear old PMDs */
    map_lowmem();                /* map physical RAM */
    dma_contiguous_remap();      /* fix CMA attributes */
    early_fixmap_shutdown();     /* rebuild fixmap */
    devicemaps_init(mdesc);      /* vectors page + mdesc->map_io() */
    kmap_init();                 /* highmem kmap PTEs */
    tcm_init();                  /* Tightly-Coupled Memory */

    zero_page = early_alloc(PAGE_SIZE);
    ...

    bootmem_init();
}
```

ARM32 passes `mdesc` because it needs `mdesc->map_io()` and `mdesc->reserve()` context.

### ARM64 paging_init() — arch/arm64/mm/mmu.c

```c
void __init paging_init(void)
{
    pgd_t *pgdp = pgd_set_fixmap(__pa_symbol(swapper_pg_dir));

    map_kernel(pgdp);       /* map kernel text+data with correct permissions */
    map_mem(pgdp);          /* map all physical memory (flat linear map) */

    pgd_clear_fixmap();

    cpu_replace_ttbr1(lm_alias(swapper_pg_dir), init_idmap_pg_dir);

    memblock_phys_free(__pa_symbol(init_pg_dir),
                       __pa_symbol(init_pg_end) - __pa_symbol(init_pg_dir));

    memblock_allow_resize();

    bootmem_init();
}
```

ARM64 does NOT pass `mdesc` (no machine descriptor). It uses a two-phase approach: `map_kernel()` sets up kernel code/data mapping, `map_mem()` sets up the flat linear map of all physical memory.

---

## 4. Key Difference: Linear Map vs Lowmem

### ARM32: Lowmem is a subset of RAM

```
Physical:  [0x00000000 - 0x3FFFFFFF] (1GB RAM)
           [0x00000000 - 0x37FFFFFF] → lowmem (directly mapped)
           [0x38000000 - 0x3FFFFFFF] → highmem (not directly mapped)

Kernel VA: 0xC0000000 - 0xF7FFFFFF → direct map of lowmem
           0xF8000000 - 0xFF800000 → vmalloc window
```

Only some RAM is directly mapped. The rest needs kmap/highmem.

### ARM64: ALL RAM is linearly mapped

```
Physical:  [0x0000000000 - 0xFFFFFFFFFF] (64GB RAM)
           ALL directly mapped

Kernel VA: 0xFFFF800000000000 - 0xFFFF840000000000→ direct map of all 64GB
           0xFFFF000000000000 - 0xFFFF7FFFFFFFFFFF → vmalloc (128TB)
```

All physical memory has a kernel virtual address via `__va(pa)`. No highmem. No kmap.

---

## 5. ARM32 Sections vs ARM64 Huge Pages

ARM32 (non-LPAE) uses 1MB "sections" for efficient lowmem mapping:

```
ARM32 page table entry for section:
  Bits [31:20] → Physical base address (1MB aligned)
  Bit  [1:0]   → 0b10 → section type
  Bits [8:5]   → Domain
  Bit  [12]    → C (cacheable)
  Bit  [3]     → B (bufferable)
  Bits [11:10] → AP (access permission)
```

A 1GB lowmem mapping requires 1024 section entries (1024 × 1MB = 1GB). Each section entry is 4 bytes → 4KB for the first-level table.

ARM64 (LPAE) uses 2MB PMD entries (hugepages):
```
ARM64 PMD huge page:
  Bits [47:21] → Physical base address (2MB aligned)
  Bit  [1]     → 1 (valid)
  Bit  [0]     → 1 (table) or 0 (block/hugepage)
  ...rest: memory attributes (MT_NORMAL, etc.)
```

Both are architecturally efficient (fewer TLB entries needed for large RAM).

---

## 6. KASLR Impact on paging_init()

### ARM32: No KASLR

ARM32 uses fixed virtual addresses:
- `PAGE_OFFSET = 0xC0000000` (fixed)
- `PHYS_OFFSET = 0x00000000` (fixed or board-specific)
- Kernel loads at exactly `PHYS_OFFSET + (KERNEL_START - PAGE_OFFSET)`

`map_lowmem()` always maps from `PHYS_OFFSET` to `arm_lowmem_limit`. Deterministic.

### ARM64: KASLR Randomizes Everything

```c
/* arch/arm64/mm/init.c */
static void __init map_mem(pgd_t *pgdp)
{
    phys_addr_t kernel_start = __pa_symbol(_text);
    phys_addr_t kernel_end   = __pa_symbol(__init_begin);
    struct memblock_region *reg;
    int flags = 0;

    /* Map the entire physical memory */
    for_each_available_child_of_node(...) {
        __map_memblock(pgdp, start, end, ...);
    }
}
```

`__pa_symbol(_text)` is randomized by KASLR — it varies at each boot. `map_mem()` handles this transparently because it derives all addresses from `memstart_addr` (the randomized physical start).

---

## 7. Vectors Page

### ARM32: Exception Vectors Need Special Mapping

ARM32 CPUs fetch exception vectors from a fixed VA (either 0x00000000 or 0xFFFF0000). This requires explicit mapping:

```c
/* devicemaps_init() — ARM32 only */
map.virtual = 0xffff0000;   /* high vectors location */
map.pfn     = __phys_to_pfn(virt_to_phys(vectors));
map.type    = MT_HIGH_VECTORS;
create_mapping(&map);

/* Copy exception handler code to the vectors page */
memcpy((void *)0xffff0000, __vectors_start, ...);
```

### ARM64: No Special Vectors Page

ARM64 CPUs fetch exception vectors from the `VBAR_EL1` register, which can point to any VA. The kernel's exception vectors (`vectors` in `arch/arm64/kernel/entry.S`) are linked at a normal kernel address and `VBAR_EL1` is set to that address. No special fixmap or dedicated vectors page.

---

## 8. Comparison Table

| Feature | ARM32 paging_init | ARM64 paging_init |
|---------|-------------------|-------------------|
| Page table levels | 2 (4+2 non-LPAE), 3 (LPAE) | 3 or 4 |
| Lowmem/highmem split | Yes | No |
| Linear map | Partial (lowmem only) | Full (all RAM) |
| KASLR | No | Yes |
| Machine descriptor | Yes (map_io) | No |
| Exception vectors | Special mapping (0xFFFF0000) | VBAR_EL1 register |
| Huge pages | 1MB sections (non-LPAE) | 2MB PMD blocks |
| TCM (Tightly-Coupled Mem) | tcm_init() | Not applicable |
| kmap_init | Yes (highmem) | Not needed |
| bootmem_init | Called from here | Called from here |
| sparse_init | Inside bootmem_init | Inside bootmem_init |
| free_area_init | Via zone_sizes_init | Same path |
