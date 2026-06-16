# `map_mem()` — Linear Map Creation Details

**Source:** `arch/arm64/mm/mmu.c` lines 1136–1206

## Purpose

`map_mem()` iterates over every memblock memory region and creates page table entries in `swapper_pg_dir` that map all physical RAM into the kernel's linear map region.

## Step-by-Step

### 1. Determine Mapping Flags

```c
int flags = NO_EXEC_MAPPINGS;

if (force_pte_mapping())
    flags |= NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS;
```

- `NO_EXEC_MAPPINGS`: All linear map entries are non-executable (the kernel's `.text` is accessible via the separate kernel image mapping)
- `NO_BLOCK_MAPPINGS`: Forces page-level (4KB) mappings. Used when `rodata=full` or `can_set_direct_map()` (needed for set_memory_* APIs)

### 2. Hide Kernel Text

```c
memblock_mark_nomap(kernel_start, kernel_end - kernel_start);
```

Temporarily marks the kernel text/rodata region as NOMAP. The `for_each_mem_range` loop skips NOMAP regions, so the kernel text won't be mapped with the regular RAM attributes.

### 3. Map All Memory Banks

```c
for_each_mem_range(i, &start, &end) {
    if (start >= end)
        break;
    __map_memblock(pgdp, start, end,
                   pgprot_tagged(PAGE_KERNEL), flags);
}
```

For each contiguous memory region, call `__map_memblock`:

```c
static void __map_memblock(pgd_t *pgdp, phys_addr_t start, phys_addr_t end,
                           pgprot_t prot, int flags)
{
    __create_pgd_mapping(pgdp, start, __phys_to_virt(start),
                         end - start, prot, pgd_pgtable_alloc, flags);
}
```

This creates the actual page table hierarchy: PGD → PUD → PMD → PTE (or block entries).

### 4. Map Kernel Text Separately

```c
__map_memblock(pgdp, kernel_start, kernel_end,
               PAGE_KERNEL, NO_CONT_MAPPINGS);
memblock_clear_nomap(kernel_start, kernel_end - kernel_start);
```

The kernel text is mapped as `PAGE_KERNEL` (RW, non-executable) in the linear map. Later, `mark_linear_text_alias_ro()` makes it read-only. `NO_CONT_MAPPINGS` ensures individual page entries so permissions can be changed per-page.

## Page Table Construction: `__create_pgd_mapping`

```c
static void __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
                                  unsigned long virt, phys_addr_t size,
                                  pgprot_t prot,
                                  phys_addr_t (*alloc)(enum pgtable_level),
                                  int flags)
```

This walks down 4 levels, allocating table pages as needed:

```
PGD (Level 0) — 512 entries, each covers 512GB
  └── PUD (Level 1) — 512 entries, each covers 1GB
        └── PMD (Level 2) — 512 entries, each covers 2MB
              └── PTE (Level 3) — 512 entries, each covers 4KB
```

### Block Descriptors

At the PUD level, if the region is 1GB-aligned:
```c
if (pud_sect_supported() &&
    ((addr | next | phys) & ~PUD_MASK) == 0 &&
    (flags & NO_BLOCK_MAPPINGS) == 0) {
    pud_set_huge(pudp, phys, prot);  // 1GB block
}
```

At the PMD level, if the region is 2MB-aligned:
```c
if (((addr | next | phys) & ~PMD_MASK) == 0 &&
    (flags & NO_BLOCK_MAPPINGS) == 0) {
    pmd_set_huge(pmdp, phys, prot);  // 2MB block
}
```

### Page Table Allocation via Memblock

```c
static phys_addr_t __init pgd_pgtable_alloc(enum pgtable_level level)
{
    phys_addr_t pa = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
    // ...
    return pa;
}
```

### Fixmap Access Pattern

Since the linear map doesn't exist yet, page table pages are accessed via fixmap:

```c
static inline pte_t *pte_set_fixmap(phys_addr_t addr)
{
    __set_fixmap(FIX_PTE, addr, PAGE_KERNEL);
    return (pte_t *)__fix_to_virt(FIX_PTE);
}

static inline void pte_clear_fixmap(void)
{
    clear_fixmap(FIX_PTE);
}
```

Each page table page is temporarily mapped into the fixmap, filled, then unmapped. This is the bootstrap chicken-and-egg solution: you can't use the linear map to build the linear map.

## Example: Mapping 2GB of RAM

```
RAM: 0x4000_0000 — 0xBFFF_FFFF (2GB)

PGD index: 0 (assuming PAGE_OFFSET maps here)
  └── PUD[0]: 1GB block → 0x4000_0000 - 0x7FFF_FFFF  ✓
  └── PUD[1]: 1GB block → 0x8000_0000 - 0xBFFF_FFFF  ✓

Result: 2 page table entries map 2GB of RAM!
```

If block mappings are disabled (`rodata=full`):
```
PGD[0]
  └── PUD[0]
        └── PMD[0]: PTE[0..511] → 2MB of 4KB pages
        └── PMD[1]: PTE[0..511] → 2MB of 4KB pages
        ... (512 PMDs)
  └── PUD[1]
        └── PMD[0]: PTE[0..511]
        ... (512 PMDs)

Result: 2 PUD + 1024 PMD + 524288 PTE entries
```

## Key Takeaway

`map_mem()` creates the linear map by walking memblock's memory list and building page tables in `swapper_pg_dir`. It uses memblock to allocate page table pages and fixmap to access them — a elegant bootstrap where the allocator and the mapping system cooperate to build themselves. Block descriptors (1GB, 2MB) minimize the number of page table entries needed.
