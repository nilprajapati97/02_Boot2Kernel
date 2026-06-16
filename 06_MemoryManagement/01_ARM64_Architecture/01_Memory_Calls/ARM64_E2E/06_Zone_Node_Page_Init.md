# 06 — Zone, Node & Page Init: `free_area_init()` Walkthrough

[← Doc 05: Page Table Chain](05_Page_Table_Chain.md) | **Doc 06** | [Doc 07: Buddy Handoff →](07_Buddy_Handoff_Code.md)

---

## Overview

After the linear map is built and memblock knows all memory, the kernel must initialize
the zone/node/page infrastructure. This happens in `mm_core_init()` → `free_area_init()`.
This document traces the exact code that creates `pglist_data` nodes, zones, and
`struct page` arrays.

---

## 1. `mm_core_init()` — The Memory Subsystem Initializer

**Source**: `mm/mm_init.c`  
**Called by**: `start_kernel()`

```c
void __init mm_core_init(void)
{
    /* ─── Build zone lists for all NUMA nodes ─── */
    build_all_zonelists(NULL);         // Construct fallback zone lists
    //   For each node, build ordered list of zones to try
    //   Order: ZONE_NORMAL → ZONE_DMA32 → ZONE_DMA (fallback)

    page_alloc_init_cpuhp();           // Register CPU hotplug callbacks

    /* ─── Report page sizes ─── */
    report_meminit();                  // Print page size, section info

    /* ─── Deferred struct page init ─── */
    kmsan_init_shadow();               // KMSAN shadow init (if configured)

    /* ─── Memory debugging ─── */
    // page_ext_init_flatmem(), mem_debugging_and_hardening_init()

    /* ─── Slab allocator ─── */
    kmem_cache_init();                 // ★ SLUB bootstrap (see Doc 08)

    /* ─── vmalloc ─── */
    vmalloc_init();                    // ★ vmalloc subsystem (see Doc 09)

    /* ─── Per-CPU page allocator ─── */
    page_alloc_init();

    /* ─── Various ─── */
    init_espfix_bsp();
    pti_init();
    execmem_init();
}
```

But the zone/page structures are initialized **earlier** in `mm_core_init_early()`:

```c
void __init mm_core_init_early(void)
{
    // ...
    free_area_init();                  // ★ THIS DOCUMENT
    // ...
}
```

---

## 2. `free_area_init()` — Top-Level Zone/Page Initialization

**Source**: `mm/mm_init.c`  
**Called by**: `mm_core_init_early()`

```c
void __init free_area_init(void)                     // mm/mm_init.c:1831
{
    unsigned long max_zone_pfns[MAX_NR_ZONES] = {0};
    int nid;

    /* ─── Step 1: Compute zone PFN limits ─── */
    arch_zone_limits_init(max_zone_pfns);
    //   ARM64 fills in:
    //     max_zone_pfns[ZONE_DMA]    = arm64_dma_phys_limit >> PAGE_SHIFT
    //     max_zone_pfns[ZONE_DMA32]  = max_zone_phys(DMA_BIT_MASK(32)) >> PAGE_SHIFT
    //     max_zone_pfns[ZONE_NORMAL] = max_pfn
```

### 2.1 `arch_zone_limits_init()` — ARM64 Zone Boundaries

```c
static void __init arch_zone_limits_init(unsigned long *max_zone_pfns)
{
    unsigned long max = max_pfn;   // From bootmem_init()

#ifdef CONFIG_ZONE_DMA
    max_zone_pfns[ZONE_DMA] = PFN_DOWN(arm64_dma_phys_limit);
    //   Example: arm64_dma_phys_limit = 0x1_0000_0000 (4GB)
    //            → ZONE_DMA PFN limit = 0x100000
#endif
#ifdef CONFIG_ZONE_DMA32
    max_zone_pfns[ZONE_DMA32] = PFN_DOWN(max_zone_phys(DMA_BIT_MASK(32)));
    //   DMA_BIT_MASK(32) = 0xFFFF_FFFF
    //   max_zone_phys(0xFFFF_FFFF) = min(4GB, end_of_DRAM)
    //   → ZONE_DMA32 PFN limit = 0x100000
#endif
    max_zone_pfns[ZONE_NORMAL] = max;
    //   → ZONE_NORMAL PFN limit = max_pfn (end of all RAM)
}
```

### 2.2 Back in `free_area_init()`

```c
    /* ─── Step 2: Determine zone ranges from max PFNs ─── */
    free_area_init_max_zone_pfns(max_zone_pfns);
    //   Computes arch_zone_lowest_possible_pfn[] and
    //   arch_zone_highest_possible_pfn[] for each zone

    /* ─── Step 3: Sparsemem initialization ─── */
    sparse_init();
    //   For SPARSEMEM_VMEMMAP (ARM64 default):
    //     1. Allocate section_mem_map for each memory section
    //     2. Populate vmemmap page tables (struct page array)
    //     3. Each section = 128 MB (SECTION_SIZE_BITS = 27)
    //     4. vmemmap entries: memblock_alloc for backing pages
    //        mapped at VMEMMAP_START virtual address

    /* ─── Step 4: Initialize each NUMA node ─── */
    for_each_node(nid) {
        pg_data_t *pgdat;

        if (!node_online(nid)) {
            /* Offline node → allocate pgdat from another node */
            pgdat = arch_alloc_nodedata(nid);
            //   memblock_alloc(sizeof(pg_data_t), SMP_CACHE_BYTES)
            pgdat->node_id = nid;
            free_area_init_memoryless_node(nid);
            continue;
        }

        pgdat = NODE_DATA(nid);
        //   NODE_DATA(0) = &contig_page_data (UMA)
        //   or = node_data[nid] (NUMA)

        free_area_init_node(nid);        // ★ Per-node initialization
        //   Sets up pgdat, zones, struct page arrays

        /* Check if this node has deferred struct page init */
        if (pgdat->node_spanned_pages)
            node_set_state(nid, N_MEMORY);
    }

    /* ─── Step 5: Estimate per-node memory for NUMA ─── */
    memblock_estimate_nr_free();
}
```

---

## 3. `free_area_init_node()` — Per-Node Setup

**Source**: `mm/mm_init.c`

```c
static void __init free_area_init_node(int nid)      // mm/mm_init.c:1725
{
    pg_data_t *pgdat = NODE_DATA(nid);
    unsigned long start_pfn = 0, end_pfn = 0;

    /* Get PFN range for this node from memblock */
    get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);
    //   Walks memblock.memory, finds min and max PFN for this node
    //   Example (UMA, node 0): start_pfn = 0x40000, end_pfn = 0xC0000

    pgdat->node_id = nid;
    pgdat->node_start_pfn = start_pfn;
    pgdat->node_spanned_pages = end_pfn - start_pfn;
    //   spanned = total PFN range including holes

    pr_info("Initmem setup node %d [mem %#018Lx-%#018Lx]\n", nid,
            (u64)start_pfn << PAGE_SHIFT,
            end_pfn ? ((u64)end_pfn << PAGE_SHIFT) - 1 : 0);
    //   Prints: "Initmem setup node 0 [mem 0x0000000040000000-0x00000000bfffffff]"

    /* Calculate per-zone page counts and holes */
    calculate_node_totalpages(pgdat, start_pfn, end_pfn);
    //   For each zone: count pages that belong to this node
    //   pgdat->node_present_pages = actual pages (excl. holes)

    free_area_init_core(pgdat);        // ★ Zone and page initialization
}
```

---

## 4. `free_area_init_core()` — Zone Data Structures

**Source**: `mm/mm_init.c`

```c
static void __init free_area_init_core(pg_data_t *pgdat)    // mm/mm_init.c:1604
{
    int nid = pgdat->node_id;
    int j;
    enum zone_type highest_zoneidx = -1;

    /* ─── Per-zone initialization ─── */
    for (j = 0; j < MAX_NR_ZONES; j++) {
        struct zone *zone = pgdat->node_zones + j;
        unsigned long size = zone->spanned_pages;
        unsigned long freesize = zone->present_pages;
        unsigned long memmap_pages;

        if (highest_zoneidx < 0 && size)
            highest_zoneidx = j;
        if (!size)
            continue;

        /* ─── Account for memmap overhead ─── */
        memmap_pages = calc_memmap_size(size, freesize);
        //   = sizeof(struct page) * freesize / PAGE_SIZE
        //   Struct page array itself consumes memory
        if (!is_highmem_idx(j)) {
            if (freesize >= memmap_pages) {
                freesize -= memmap_pages;
                if (memmap_pages)
                    pr_debug("  %s zone: %lu pages used for memmap\n",
                             zone_names[j], memmap_pages);
            }
        }

        /* ─── Account for DMA reserve ─── */
        if (j == ZONE_DMA || j == ZONE_DMA32)
            freesize -= min(freesize, dma_reserve);

        /* ─── Set managed pages (initially = present - overhead) ─── */
        if (!is_highmem_idx(j))
            nr_kernel_pages += freesize;
        else
            nr_all_pages += freesize;
        nr_all_pages += freesize;

        zone->managed_pages = freesize;
        //   managed_pages = pages available for allocation
        //   (This gets updated again when pages are freed to buddy)

        /* ─── Pageblock bitmap ─── */
        setup_usemap(zone);
        //   Allocate pageblock_flags bitmap from memblock:
        //     nr_pageblocks = DIV_ROUND_UP(spanned_pages, pageblock_nr_pages)
        //     usemap_size = BITS_TO_LONGS(nr_pageblocks * NR_PAGEBLOCK_BITS) * sizeof(long)
        //     zone->pageblock_flags = memblock_alloc(usemap_size, ...)

        /* ─── Core zone initialization ─── */
        zone_init_internals(zone, j, nid, freesize);
        //   zone->name = zone_names[j]
        //   zone->zone_pgdat = pgdat
        //   spin_lock_init(&zone->lock)
        //   zone_seqlock_init(zone)
        //   zone_pcp_init(zone)

        /* ─── Free lists ─── */
        if (!size)
            continue;
        set_pageblock_order();
        //   Sets pageblock_order (usually MAX_PAGE_ORDER = 10 = 4MB)

        zone_init_free_lists(zone);
        //   Initialize empty free lists for each order

        /* ─── Initialize struct page entries ─── */
        if (!early_page_uninitialised(zone_start_pfn(zone)))
            memmap_init(zone, size);   // ★ Initialize struct page
    }

    pgdat_init_internals(pgdat);
    //   spin_lock_init(&pgdat->lru_lock)
    //   lruvec_init(&pgdat->__lruvec)
    //   pgdat->kswapd_order = 0
    //   init_waitqueue_head(&pgdat->kswapd_wait)
    //   init_waitqueue_head(&pgdat->pfmemalloc_wait)
}
```

---

## 5. `zone_init_free_lists()` — Empty Free Lists

**Source**: `mm/mm_init.c`

```c
static void __init zone_init_free_lists(struct zone *zone)    // mm/mm_init.c:1444
{
    unsigned int order, t;

    for_each_migratetype_order(order, t) {
        //   order: 0 to MAX_PAGE_ORDER (10)
        //   t: MIGRATE_UNMOVABLE, MIGRATE_MOVABLE, MIGRATE_RECLAIMABLE,
        //      MIGRATE_HIGHATOMIC, MIGRATE_CMA, MIGRATE_ISOLATE

        INIT_LIST_HEAD(&zone->free_area[order].free_list[t]);
        //   Initialize empty doubly-linked list
        zone->free_area[order].nr_free = 0;
        //   Zero free pages at this order
    }
}
```

**Data structure**:
```c
struct zone {
    // ...
    struct free_area free_area[NR_PAGE_ORDERS];   // orders 0..MAX_PAGE_ORDER
    // ...
};

struct free_area {
    struct list_head free_list[MIGRATE_TYPES];     // One list per migrate type
    unsigned long    nr_free;                      // Total free pages at this order
};
```

At this point, ALL free lists are EMPTY. Pages will be freed into them later
by `memblock_free_all()` (Doc 07).

---

## 6. `memmap_init()` — Initialize `struct page` for Each PFN

**Source**: `mm/mm_init.c`

```c
void __init memmap_init(struct zone *zone, unsigned long size)   // mm/mm_init.c:977
{
    unsigned long start_pfn = zone->zone_start_pfn;
    unsigned long end_pfn = start_pfn + size;  // spanned_pages
    unsigned long pfn;

    if (highest_memmap_pfn < end_pfn - 1)
        highest_memmap_pfn = end_pfn - 1;

    for (pfn = start_pfn; pfn < end_pfn; ) {
        if (overlap_memmap_init(zone, &pfn))
            continue;                           // Skip deferred init ranges

        if (context_tracking_in_user() || pfn_valid(pfn)) {
            memmap_init_range(pfn, min(pfn + PAGES_PER_SECTION, end_pfn),
                              zone_to_nid(zone), zone, 0, MEMINIT_EARLY);
        } else {
            pfn += PAGES_PER_SECTION;
        }
    }
}
```

### 6.1 `memmap_init_range()` — Per-Page Setup

```c
void __init memmap_init_range(unsigned long start_pfn, unsigned long end_pfn,
                              int nid, struct zone *zone,
                              unsigned long zone_pgdat_pfn,
                              enum meminit_context context)
{
    unsigned long pfn;

    for (pfn = start_pfn; pfn < end_pfn; pfn++) {
        struct page *page;

        if (!early_pfn_in_nid(pfn, nid))
            continue;                 // Skip pages not on this node

        if (!pfn_valid(pfn))
            continue;                 // Skip holes

        page = pfn_to_page(pfn);
        //   SPARSEMEM_VMEMMAP:
        //     page = vmemmap + pfn
        //     vmemmap = (struct page *)VMEMMAP_START
        //     Each struct page = 64 bytes

        __init_single_page(page, pfn, zone_idx(zone), nid);
        //   page->flags = 0
        //   set_page_zone(page, zone_idx)     → encode zone in flags
        //   set_page_node(page, nid)          → encode node in flags
        //   page_mapcount_reset(page)         → page->_mapcount = -1
        //   page_cpupid_reset_last(page)
        //   page_kasan_tag_reset(page)
        //   init_page_count(page)             → page->_refcount = 1
        //   INIT_LIST_HEAD(&page->lru)
        //   SetPageReserved(page)             → page->flags |= PG_reserved
        //                                     ★ ALL pages start RESERVED

        /* Mark pageblock type for the first page of each pageblock */
        if (pageblock_aligned(pfn)) {
            set_pageblock_migratetype(page, MIGRATE_MOVABLE);
            //   Write to zone->pageblock_flags bitmap
            //   Default: all pageblocks start as MOVABLE
        }
    }
}
```

**Critical**: Every `struct page` starts with:
- `_refcount = 1` (held)
- `PG_reserved` set (not free)
- Zone and node encoded in `page->flags`

The `PG_reserved` flag is cleared and `_refcount` set to 0 when pages are freed
to the buddy allocator (Doc 07).

---

## 7. `sparse_init()` — SPARSEMEM Sections and vmemmap

**Source**: `mm/sparse.c`

```c
void __init sparse_init(void)
{
    unsigned long pnum_begin, pnum_end;

    /* Allocate mem_section[] array (in-place or from memblock) */

    /* Populate each section that has memory */
    for_each_present_section_nr(pnum_begin, pnum_end) {
        sparse_init_nid(section_to_node_table[pnum],
                       pnum_begin, pnum_end);
    }
    vmemmap_populate_print_last();
}
```

For SPARSEMEM_VMEMMAP, `struct page` arrays are mapped into the vmemmap
virtual address range (`VMEMMAP_START`), backed by actual physical pages
allocated from memblock. The `pfn_to_page()` macro simply adds the PFN
to the vmemmap base:

```c
#define pfn_to_page(pfn)    (vmemmap + (pfn))
#define page_to_pfn(page)   ((unsigned long)(page) - (unsigned long)vmemmap)
```

---

## State After Document 06

```
┌──────────────────────────────────────────────────────────┐
│  Zone/Node/Page Infrastructure Ready:                    │
│                                                          │
│  NUMA Nodes:                                             │
│    pg_data_t for each online node                        │
│    node_start_pfn, node_spanned_pages, node_present_pages│
│                                                          │
│  Zones (per node):                                       │
│    ZONE_DMA    [0 .. dma_limit_pfn)                      │
│    ZONE_DMA32  [dma_limit_pfn .. 4GB_pfn)                │
│    ZONE_NORMAL [4GB_pfn .. max_pfn)                      │
│                                                          │
│    Each zone has:                                        │
│    ✓ free_area[0..MAX_ORDER] with empty free lists       │
│    ✓ pageblock_flags bitmap allocated                    │
│    ✓ zone->lock, pcp, waitqueues initialized             │
│    ✓ managed_pages estimated                             │
│                                                          │
│  Struct page array:                                      │
│    ✓ SPARSEMEM_VMEMMAP: vmemmap populated                │
│    ✓ Every valid struct page initialized:                │
│       - _refcount = 1                                    │
│       - PG_reserved = SET                                │
│       - zone, node encoded in flags                      │
│       - pageblock type = MIGRATE_MOVABLE                 │
│                                                          │
│  Free lists: ALL EMPTY                                   │
│    Pages are still "owned" by memblock                   │
│    No buddy allocator yet                                │
│                                                          │
│  Next: memblock_free_all() hands pages to buddy          │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 05: Page Table Chain](05_Page_Table_Chain.md) | **Doc 06** | [Doc 07: Buddy Handoff →](07_Buddy_Handoff_Code.md)
