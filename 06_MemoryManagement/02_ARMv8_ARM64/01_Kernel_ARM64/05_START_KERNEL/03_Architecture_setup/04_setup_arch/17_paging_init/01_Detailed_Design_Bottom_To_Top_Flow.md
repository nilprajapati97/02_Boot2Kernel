# paging_init() — Detailed Design

## 1. Position and Significance

`paging_init()` is the **single most important function** in ARM32 memory initialization. Before it runs, the CPU is using page tables set up by head.S (identity mapping only, minimal coverage). After it runs, the full kernel virtual memory layout is in place and the CPU is running with the permanent page tables.

```
setup_arch():
  ├── early_ioremap_reset()   ← fixmap transition done
  └── paging_init(mdesc)      ← *** THIS FUNCTION ***
        │ Builds the kernel's permanent page table hierarchy
        │ Maps all lowmem, kernel code, device memory
        │ Initializes memory zones and buddy allocator
        ├── prepare_page_table()
        ├── map_lowmem()
        ├── map_kernel()          (actually embedded in map_lowmem on ARM32)
        ├── dma_contiguous_remap()
        ├── early_fixmap_shutdown()
        ├── devicemaps_init(mdesc)
        ├── kmap_init()
        ├── tcm_init()
        └── bootmem_init()
              ├── sparse_init()
              └── zone_sizes_init() → free_area_init()
```

---

## 2. Step 1: prepare_page_table()

**File:** `arch/arm/mm/mmu.c`

```c
static inline void prepare_page_table(void)
{
    unsigned long addr;
    phys_addr_t end;

    /*
     * Clear out all the mappings below the kernel image.
     */
    for (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)
        pmd_clear(pmd_off_k(addr));

#ifdef CONFIG_XIP_KERNEL
    /* XIP kernel: different handling */
#else
    for (; addr < PAGE_OFFSET; addr += PMD_SIZE)
        pmd_clear(pmd_off_k(addr));
#endif

    /*
     * Find the end of the first block of lowmem.
     */
    end = memblock_end_of_DRAM();
    if (end >= arm_lowmem_limit)
        end = arm_lowmem_limit;

    /*
     * Clear out all the kernel mappings from arm_lowmem_limit to VMALLOC_START.
     */
    for (addr = __phys_to_virt(end); addr < VMALLOC_START; addr += PMD_SIZE)
        pmd_clear(pmd_off_k(addr));
}
```

This clears ALL PMD entries:
1. From VA 0x00000000 to `MODULES_VADDR` (user VA range — no kernel mappings here)
2. From `arm_lowmem_limit` VA to `VMALLOC_START` (the VMALLOC_OFFSET gap — must be unmapped)

After this, the kernel is running with only the head.S identity map for the currently-executing code. Any access to other VAs would fault. This is safe because `prepare_page_table()` runs from code that's in the still-valid mapped range.

---

## 3. Step 2: map_lowmem()

```c
static void __init map_lowmem(void)
{
    phys_addr_t kernel_x_start = round_down(__pa(KERNEL_START), SECTION_SIZE);
    phys_addr_t kernel_x_end   = round_up(__pa(__init_end), SECTION_SIZE);
    phys_addr_t start, end;
    u64 i;

    /* Map all available physical memory (memblock) as lowmem */
    for_each_mem_range(i, &start, &end) {
        struct map_desc map;

        if (end > arm_lowmem_limit)
            end = arm_lowmem_limit;
        if (start >= end)
            break;

        if (end < kernel_x_start || start >= kernel_x_end) {
            /* Region entirely outside kernel: map as RW (no execute) */
            map.pfn   = __phys_to_pfn(start);
            map.virtual = __phys_to_virt(start);
            map.length = end - start;
            map.type   = MT_MEMORY_RWX;
            create_mapping(&map);
        } else {
            /* Region overlaps kernel: split into segments */
            /* Before kernel: RW */
            /* Kernel text: RWX (execute needed for running code) */
            /* After __init_end: RW */
        }
    }
}
```

`map_lowmem()` creates page table entries for all physical memory below `arm_lowmem_limit`, mapping it to kernel virtual space starting at `PAGE_OFFSET`. The kernel code region gets execute permission; data and other regions get RW only.

**Memory type selection:**
- `MT_MEMORY_RWX`: Normal cacheable memory, read/write/execute
- `MT_MEMORY_RW`: Normal cacheable memory, read/write (no execute) — W^X compliance

---

## 4. Step 3: dma_contiguous_remap()

```c
void __init dma_contiguous_remap(void)
{
    /* Remap CMA regions as non-cached (MT_MEMORY_RW vs MT_MEMORY_RWX) */
    /* CMA regions don't need execute permission */
    if (size == 0)
        return;

    base = pfn_to_phys(pfn);
    remap_area_pte(base, size, MT_MEMORY_RW);
}
```

CMA regions are remapped without execute permission and with appropriate cache attributes. The CMA region was previously mapped by `map_lowmem()` as part of the general lowmem mapping, but the CMA remap fine-tunes the attributes.

---

## 5. Step 4: early_fixmap_shutdown()

```c
static void __init early_fixmap_shutdown(void)
{
    /* Re-establish permanent fixmap PTEs in the new page tables */
    /* Any fixmap slots still active (e.g., earlycon) are remapped
       using the permanent PTE allocations */
    early_ioremap_shutdown();
    fixmap_pmd_init();    /* allocate permanent PTE page for fixmap PMD */
}
```

After `prepare_page_table()` cleared all PMDs, the fixmap PMD entry was destroyed. `early_fixmap_shutdown()` re-creates it using the new permanent page table infrastructure.

---

## 6. Step 5: devicemaps_init(mdesc)

```c
static void __init devicemaps_init(const struct machine_desc *mdesc)
{
    struct map_desc map;
    unsigned long addr;
    void *vectors;

    /* Allocate the vector page */
    vectors = early_alloc(PAGE_SIZE * 2);

    /* Map VECTORS_BASE (0xFFFF0000 on ARM32) */
    map.pfn   = __phys_to_pfn(virt_to_phys(vectors));
    map.virtual = 0xffff0000;
    map.length = PAGE_SIZE;
    map.type   = MT_HIGH_VECTORS;
    create_mapping(&map);

    /* Copy exception vectors to the vector page */
    memcpy((void *)MAP_HIGH_VECTORS_BASE, __vectors_start,
           __vectors_end - __vectors_start);
    memcpy((void *)MAP_HIGH_VECTORS_BASE + 0x1000, __stubs_start,
           __stubs_end - __stubs_start);

    /* Map any machine-specific I/O areas */
    if (mdesc->map_io)
        mdesc->map_io();

    /* Map flush_icache_all / other special pages */
    ...
}
```

ARM32 exception vectors live at a fixed VA (0xFFFF0000 with high vectors, or 0x00000000). `devicemaps_init()` allocates physical pages, maps them, and copies the exception handler code there.

The `mdesc->map_io()` callback allows boards to set up static MMIO mappings (e.g., UART registers used throughout boot) in the permanent page tables.

---

## 7. Step 6: kmap_init()

```c
void __init kmap_init(void)
{
    /* Set up page table entries for the kmap region (if CONFIG_HIGHMEM) */
    pkmap_page_table = early_pte_alloc(pmd_off_k(PKMAP_BASE),
                                       PKMAP_BASE, _PAGE_KERNEL);
}
```

kmap allows the kernel to temporarily map highmem pages into a small VA window (PKMAP region). `kmap_init()` pre-allocates the PTE page for this region.

---

## 8. Step 7: bootmem_init() → sparse_init() → zone_sizes_init()

```c
void __init bootmem_init(void)
{
    unsigned long min, max_low, max_high;

    memblock_allow_resize();
    max_low = max_high = 0;

    find_limits(&min, &max_low, &max_high);
    early_memtest((phys_addr_t)min << PAGE_SHIFT,
                  (phys_addr_t)max_low << PAGE_SHIFT);

    /* Initialize sparse memory */
    sparse_init();

    zone_sizes_init(min, max_low, max_high);
}
```

### sparse_init()

Initializes the sparse memory model (used on ARM for memory hotplug and holes). Creates `struct mem_section` arrays that track which memory sections (128MB chunks) are present and where their `struct page` arrays live.

### zone_sizes_init() → free_area_init()

```c
static void __init zone_sizes_init(unsigned long min, unsigned long max_low,
                                   unsigned long max_high)
{
    unsigned long max_zone_pfn[MAX_NR_ZONES] = { 0 };

    max_zone_pfn[ZONE_NORMAL] = max_low;
#ifdef CONFIG_HIGHMEM
    max_zone_pfn[ZONE_HIGHMEM] = max_high;
#endif
    free_area_init(max_zone_pfn);
}
```

`free_area_init()` initializes the per-zone data structures:
- `struct zone` for ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM
- `struct page` array (`mem_map`) — one struct per physical page frame
- Free lists for each zone
- Per-zone per-order free area lists (buddy allocator)

After `free_area_init()`, the zone and page structures exist, but all pages are marked as "reserved" (not yet in the free lists). Pages are added to free lists by `mem_init()` (called later from `start_kernel()`).

---

## 9. The Complete Call Tree

```
paging_init(mdesc)
├── prepare_page_table()        — clear old PMDs
├── map_lowmem()                — map physical RAM as lowmem
│     └── create_mapping()     — builds PGD+PMD+PTE entries
├── dma_contiguous_remap()      — fix CMA page attributes
├── early_fixmap_shutdown()     — rebuild fixmap in new tables
├── devicemaps_init(mdesc)      — vectors page, mdesc->map_io()
├── kmap_init()                 — highmem kmap PTE page
├── tcm_init()                  — Tightly-Coupled Memory (rare)
└── bootmem_init()
      ├── sparse_init()
      └── zone_sizes_init()
            └── free_area_init()   — create zone/page structures
```

---

## 10. Interview Q&A

**Q1: What is the difference between the identity mapping set up by head.S and the mapping built by paging_init()?**
> head.S sets up a minimal identity mapping (PA=VA) for the kernel load address, enabling the CPU to continue running after enabling the MMU. It's temporary — just enough to get from assembly to C. `paging_init()` builds the full permanent mapping: kernel VA at PAGE_OFFSET maps to physical addresses starting at PHYS_OFFSET, with correct permissions (RW for data, RWX for text), covering all of lowmem plus fixmap, vectors, etc.

**Q2: After paging_init() returns, can the identity mapping still be accessed?**
> On ARM32, the identity mapping section (the 1MB section covering the kernel load address) is kept active while the kernel transitions from identity-mapping code to PAGE_OFFSET-mapped code. Once the kernel is running from its PAGE_OFFSET virtual addresses, `prepare_page_table()` would have cleared the identity-mapping PMD entries (they're in the 0x00000000–PAGE_OFFSET range, which is user space). Accessing those VAs after `paging_init()` gives a fault.

**Q3: Why does map_lowmem() map kernel text as RWX initially?**
> During `map_lowmem()`, the kernel marks executable sections with execute permission. Later, `mark_rodata_ro()` (called during `kernel_init()`) removes write permission from read-only sections and removes execute permission from non-text sections. This is the W^X (write XOR execute) enforcement. During early boot, the kernel can't mark sections RO yet because `__init` sections are still in use. The full W^X enforcement happens after the kernel's own init code completes.

**Q4: What is mem_map and how is it created by paging_init()?**
> `mem_map` is the global array of `struct page` — one entry per physical page frame. Each `struct page` holds metadata about one 4KB page: its flags (allocated/free/locked/etc.), reference count, mapping pointer, and list links. `free_area_init()` inside `bootmem_init()` allocates the `mem_map` array using memblock (because the buddy allocator isn't available yet). The array is sized to cover all physical pages from min_pfn to max_pfn. After this, `pfn_to_page(pfn)` and `page_to_pfn(page)` work correctly.

**Q5: What does zone_sizes_init() set as the boundary between ZONE_NORMAL and ZONE_HIGHMEM?**
> `zone_sizes_init()` is called with `max_low` (the highest lowmem PFN) and `max_high` (the highest PFN total). On ARM32, `max_low` = `arm_lowmem_limit >> PAGE_SHIFT`. `max_zone_pfn[ZONE_NORMAL] = max_low`. Any physical page above `max_low` PFN is in `ZONE_HIGHMEM`. These pages are not directly mapped — the kernel accesses them via `kmap()`. When `CONFIG_HIGHMEM=n`, `max_zone_pfn[ZONE_HIGHMEM] = 0` and no highmem zone exists.
