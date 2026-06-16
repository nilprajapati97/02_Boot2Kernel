# 07 — Buddy Handoff: `memblock_free_all()` Code Walkthrough

[← Doc 06: Zone, Node & Page Init](06_Zone_Node_Page_Init.md) | **Doc 07** | [Doc 08: SLUB Bootstrap →](08_SLUB_Bootstrap_Code.md)

---

## Overview

This is the pivotal transition: all free physical memory moves from memblock's
reserved/memory arrays into the buddy allocator's free lists. After this, memblock
is retired and `alloc_pages()` becomes the primary page allocator.

---

## 1. When Does It Happen?

**Source**: `mm/mm_init.c`

```c
void __init page_alloc_init_late(void)           // mm/mm_init.c:2323
{
    // Called from kernel_init_freeable() after most of start_kernel()

    set_pageblock_migratetype_for_cma();          // Mark CMA pageblocks
    struct_pages_for_free_area_init();            // Deferred struct page init

    memblock_free_all();                          // ★ THE HANDOFF
    //   All unreserved memblock memory → buddy free lists

    after_bootmem = 1;                            // Flag: memblock era is over

    // Post-handoff tasks
    page_alloc_sysctl_init();
    report_meminit();
}
```

---

## 2. `memblock_free_all()` — Top Level

**Source**: `mm/memblock.c`

```c
void __init memblock_free_all(void)               // mm/memblock.c:2355
{
    unsigned long pages;

    /* ─── Step 1: Reset managed_pages to 0 for all zones ─── */
    reset_all_zones_managed_pages();
    //   for_each_zone(zone):
    //       atomic_long_set(&zone->managed_pages, 0)
    //   Will be incremented as pages are freed to buddy

    /* ─── Step 2: Mark reserved pages in memmap ─── */
    memmap_init_reserved_pages();
    //   Walk memblock.reserved: for every reserved PFN,
    //     set_pageblock_migratetype(page, MIGRATE_UNMOVABLE)
    //   Reserved memory stays permanently out of buddy

    /* ─── Step 3: Free all non-reserved memory to buddy ─── */
    pages = free_low_memory_core_early();
    //   ★ THE MAIN WORK — returns count of freed pages

    totalram_pages_add(pages);
    //   atomic_long_add(&_totalram_pages, pages)
    //   Example: 2GB RAM - reserved ≈ 500K pages freed
}
```

---

## 3. `free_low_memory_core_early()` — Walk and Free

**Source**: `mm/memblock.c`

```c
static unsigned long __init free_low_memory_core_early(void)   // mm/memblock.c:2308
{
    unsigned long count = 0;
    phys_addr_t start, end;
    u64 i;

    memblock_clear_hotplug(0, -1);    // Clear hotplug flags for all memory

    /* Walk all FREE ranges (in memory but NOT in reserved) */
    for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end, NULL) {
        //   Same iterator as memblock_find_in_range_node
        //   Returns gaps between reserved regions

        count += __free_memory_core(start, end);
        //   Free this physical range to buddy
    }
    return count;
}
```

### 3.1 `__free_memory_core()` — Decompose into Power-of-2 Chunks

```c
static unsigned long __init __free_memory_core(phys_addr_t start,
                                               phys_addr_t end)
{
    unsigned long start_pfn = PFN_UP(start);      // Round UP to next PFN
    unsigned long end_pfn = PFN_DOWN(end);         // Round DOWN
    //   Partial pages at boundaries are NOT freed

    return __free_pages_memory(start_pfn, end_pfn);
}
```

### 3.2 `__free_pages_memory()` — Power-of-2 Decomposition

```c
static unsigned long __init __free_pages_memory(unsigned long start,
                                                unsigned long end)
{
    unsigned long count = 0;
    int order;

    while (start < end) {
        /*
         * Find the largest power-of-2 block that:
         * 1. Starts at a naturally aligned address (start % (1<<order) == 0)
         * 2. Doesn't exceed MAX_PAGE_ORDER (10, i.e., 4MB)
         * 3. Fits within the remaining range [start, end)
         */
        order = min_t(int, MAX_PAGE_ORDER, __ffs(start));
        //   __ffs(start) = bit position of lowest set bit in start PFN
        //   This ensures natural alignment

        while (order && (start + (1UL << order) > end))
            order--;
        //   Shrink if it would exceed the range

        memblock_free_pages(pfn_to_page(start), start, order);
        //   ★ Free 2^order pages to buddy

        count += 1UL << order;
        start += 1UL << order;
    }
    return count;
}
```

**Example**: Free range PFN 0x40200 to 0x40600 (4MB = 1024 pages):

```
Iteration 1: start=0x40200
  __ffs(0x40200) = 9 (bit 9 is lowest set)
  order = min(10, 9) = 9 → 512 pages (2MB)
  start + 512 = 0x40400 ≤ 0x40600 ✓
  → Free 512 pages at PFN 0x40200, order=9

Iteration 2: start=0x40400
  __ffs(0x40400) = 10 (bit 10)
  order = min(10, 10) = 10 → 1024 pages (4MB)
  start + 1024 = 0x40800 > 0x40600 ✗
  order-- = 9 → 512 pages
  start + 512 = 0x40600 ≤ 0x40600 ✓
  → Free 512 pages at PFN 0x40400, order=9

Total: 1024 pages freed (2 × order-9 blocks)
```

---

## 4. `memblock_free_pages()` → `__free_pages_core()`

**Source**: `mm/mm_init.c`

```c
void __init memblock_free_pages(struct page *page, unsigned long pfn,
                                unsigned int order)          // mm/mm_init.c:2492
{
    if (!early_page_test_and_clear_free(pfn, order))
        return;
    //   Deferred init check — skip if struct page not initialized

    __free_pages_core(page, order);
}
```

### 4.1 `__free_pages_core()` — Prepare Pages and Free

```c
void __init __free_pages_core(struct page *page, unsigned int order,
                              enum meminit_context context)  // mm/mm_init.c:1589
{
    unsigned int nr_pages = 1 << order;
    struct page *p = page;
    unsigned int loop;

    /*
     * When boot-time struct page content is compatible with
     * what free pages should look like, we can skip the full
     * initialization.
     */
    prefetchw(p);

    for (loop = 0; loop < (nr_pages - 1); loop++, p++) {
        prefetchw(p + 1);
        __ClearPageReserved(p);           // Clear PG_reserved flag
        set_page_count(p, 0);             // _refcount = 0 (free page)
    }
    __ClearPageReserved(p);               // Last page
    set_page_count(p, 0);                 // _refcount = 0

    /* Increment zone's managed_pages counter */
    adjust_managed_page_count(page, nr_pages);
    //   atomic_long_add(&page_zone(page)->managed_pages, nr_pages)

    set_page_refcounted(page);            // Set _refcount = 1 (for __free_pages)

    __free_pages(page, order);            // ★ Enter the buddy free path
}
```

**Page state transition**:
```
Before:  page->_refcount = 1, PG_reserved SET     (memblock owned)
  ↓ Clear PG_reserved, set _refcount = 0 for all pages in block
  ↓ Set head page _refcount = 1
  ↓ Call __free_pages() → decrements _refcount to 0 → free to buddy
After:   page->_refcount = 0, PG_reserved CLEAR   (buddy owned)
```

---

## 5. `__free_pages()` → `free_one_page()` → `__free_one_page()`

### 5.1 `__free_pages()` → `__free_pages_ok()`

```c
void __free_pages(struct page *page, unsigned int order)
{
    if (put_page_testzero(page))     // Decrement _refcount: 1→0 → true
        free_unref_page(page, order);
}
```

`free_unref_page()` leads to `free_one_page()` for boot-time (no PCP path):

### 5.2 `free_one_page()` — Zone Lock and Buddy Entry

```c
static void free_one_page(struct zone *zone, struct page *page,
                          unsigned long pfn, unsigned int order,
                          fpi_t fpi_flags)               // mm/page_alloc.c:1543
{
    unsigned long flags;

    spin_lock_irqsave(&zone->lock, flags);

    split_large_buddy(zone, page, pfn, order, fpi_flags);
    //   For initial boot free: just calls __free_one_page()
    //   (split_large_buddy handles compound page decomposition)

    spin_unlock_irqrestore(&zone->lock, flags);
}
```

### 5.3 `__free_one_page()` — The Buddy Merge Algorithm

**Source**: `mm/page_alloc.c`  
**This is the core buddy system — XOR-based buddy merging.**

```c
static inline void __free_one_page(struct page *page, unsigned long pfn,
                                   struct zone *zone, unsigned int order,
                                   int migratetype,
                                   fpi_t fpi_flags)      // mm/page_alloc.c:944
{
    struct capture_control *capc = task_capc(zone);
    unsigned long buddy_pfn;
    unsigned long combined_pfn;
    struct page *buddy;

    VM_BUG_ON(!zone_is_initialized(zone));

    /* Set the page's order in page->private */
    max_order = min_t(unsigned int, MAX_PAGE_ORDER, pageblock_order);

    while (order < max_order) {
        int buddy_mt = migratetype;

        if (compaction_capture(capc, page, order, migratetype)) {
            __mod_zone_freepage_state(zone, -(1UL << order), migratetype);
            return;
        }

        /* ─── Find buddy ─── */
        buddy_pfn = __find_buddy_pfn(pfn, order);
        //   buddy_pfn = pfn ^ (1 << order)
        //   XOR flips the bit at position 'order'
        //   Example: pfn=0x40200, order=9
        //            buddy_pfn = 0x40200 ^ 0x200 = 0x40000

        buddy = page + (buddy_pfn - pfn);
        //   buddy = page + offset to buddy's struct page

        /* ─── Is buddy free and at the same order? ─── */
        if (!page_is_buddy(page, buddy, order))
            break;
        //   Checks:
        //   1. buddy is in same zone
        //   2. buddy is NOT reserved (PG_reserved not set)
        //   3. buddy's order == our order (page_private(buddy) == order)
        //   4. buddy _refcount == 0
        //   If any check fails → can't merge, stop

        /* ─── Merge with buddy ─── */
        if (page_is_guard(buddy))
            clear_page_guard(zone, buddy, order);
        else
            __del_page_from_free_list(buddy, zone, order, buddy_mt);
        //   Remove buddy from its free list at current order

        combined_pfn = buddy_pfn & pfn;
        //   combined_pfn = lower of the two PFNs
        //   Example: 0x40000 & 0x40200 = 0x40000

        page = page + (combined_pfn - pfn);
        pfn = combined_pfn;
        order++;
        //   Now we have a 2× larger block at the combined PFN
        //   Loop again to try merging at the next order
    }

done_merging:
    set_buddy_order(page, order);
    //   page->private = order (store the final merged order)

    __add_to_free_list(page, zone, order, migratetype, false);
    //   list_add(&page->buddy_list, &zone->free_area[order].free_list[migratetype])
    //   zone->free_area[order].nr_free++
}
```

### 5.4 Buddy Merge Example

```
Free PFN 0x40200 at order 9 (512 pages):

Order 9: buddy_pfn = 0x40200 ^ 0x200 = 0x40000
  Is PFN 0x40000 free at order 9? YES
  → Merge! Remove 0x40000 from free_list[9]
  → combined_pfn = 0x40000 & 0x40200 = 0x40000
  → Now have order 10 block at PFN 0x40000

Order 10: MAX_PAGE_ORDER reached → stop
  → set_buddy_order(pfn_to_page(0x40000), 10)
  → __add_to_free_list(page, zone, 10, MIGRATE_MOVABLE, false)
  → zone->free_area[10].nr_free++

Result: One order-10 block (4MB) on the free list
```

---

## 6. `__add_to_free_list()` — Placing on Free List

```c
static inline void __add_to_free_list(struct page *page, struct zone *zone,
                                      unsigned int order, int migratetype,
                                      bool tail)             // mm/page_alloc.c:798
{
    struct free_area *area = &zone->free_area[order];

    VM_WARN_ONCE(get_pageblock_migratetype(page) != migratetype, ...);

    if (tail)
        list_add_tail(&page->buddy_list, &area->free_list[migratetype]);
    else
        list_add(&page->buddy_list, &area->free_list[migratetype]);
    //   Add to HEAD of the list (LIFO for hot pages)

    area->nr_free++;
}
```

**Free list structure after boot** (example, 2GB RAM):
```
zone->free_area[10].free_list[MIGRATE_MOVABLE]:
  → page(0x40000) → page(0x40400) → ... → page(0xBFC00)
  nr_free = ~500 (500 × 4MB ≈ 2GB)

zone->free_area[0..9]: mostly empty
  (a few smaller blocks at boundaries where buddy merging was partial)
```

---

## 7. `alloc_pages()` — The Buddy Allocator (Allocation Path)

After `memblock_free_all()`, page allocation uses the buddy allocator:

```c
struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)  // mm/page_alloc.c:5257
{
    struct page *page = __alloc_frozen_pages_noprof(gfp, order, ...);
    if (page)
        page_ref_unfreeze(page, 1);    // Set _refcount = 1
    return page;
}
```

### 7.1 Fast Path: `get_page_from_freelist()`

```c
static struct page *get_page_from_freelist(gfp_t gfp_mask, unsigned int order,
                                           int alloc_flags,
                                           const struct alloc_context *ac)
{
    struct zone *zone;
    struct page *page;

    /* Walk preferred zone list (ordered by distance from requesting node) */
    for_next_zone_zonelist_nodemask(zone, z, ac->highest_zoneidx, ac->nodemask) {

        /* Check watermarks */
        if (!zone_watermark_fast(zone, order, mark, ac->highest_zoneidx,
                                 alloc_flags, gfp_mask))
            continue;    // Zone too depleted → try next zone

        page = rmqueue(zone, order, gfp_mask, alloc_flags, ac->migratetype);
        if (page)
            return page;
    }
    return NULL;
}
```

### 7.2 `rmqueue()` → `rmqueue_buddy()` → `__rmqueue()` → `__rmqueue_smallest()`

```c
static inline struct page *__rmqueue_smallest(struct zone *zone,
                                              unsigned int order,
                                              int migratetype)  // mm/page_alloc.c:1890
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    /* Walk up from requested order to find a free block */
    for (current_order = order; current_order < NR_PAGE_ORDERS; ++current_order) {
        area = &zone->free_area[current_order];

        page = get_page_from_free_area(area, migratetype);
        //   list_first_entry_or_null(&area->free_list[migratetype], ...)

        if (!page)
            continue;    // No free blocks at this order → try larger

        /* Found a block! Remove from free list */
        page_del_and_expand(zone, page, order, current_order, area, migratetype);
        //   1. del_page_from_free_list(page, zone, current_order)
        //   2. expand(zone, page, order, current_order, migratetype)
        //      → Split larger block, put remainders back

        return page;
    }
    return NULL;
}
```

### 7.3 `expand()` — Splitting Larger Blocks

```c
static inline void expand(struct zone *zone, struct page *page,
                          int low, int high, int migratetype)  // mm/page_alloc.c:1703
{
    unsigned long size = 1 << high;

    while (high > low) {
        high--;
        size >>= 1;    // Half the block

        /* The upper half becomes a free buddy at one order lower */
        __add_to_free_list(&page[size], zone, high, migratetype, false);
        set_buddy_order(&page[size], high);

        /* Continue with the lower half at one order lower */
    }
    /* Return the lower half at the requested order */
}
```

**Split example**: Request order 0 (1 page), only order 10 available:

```
Order 10: [page+0 ........................... page+1023]
  Split → upper half [page+512..page+1023] → free_list[9]
Order 9:  [page+0 ............. page+511]
  Split → upper half [page+256..page+511] → free_list[8]
Order 8:  [page+0 ...... page+255]
  Split → upper half [page+128..page+255] → free_list[7]
  ...
Order 1:  [page+0, page+1]
  Split → page+1 → free_list[0]
Order 0:  [page+0]  ← RETURNED TO CALLER

Result: 1 page allocated, 10 smaller buddies on free lists
```

---

## State After Document 07

```
┌──────────────────────────────────────────────────────────┐
│  Buddy Allocator Active:                                 │
│                                                          │
│  memblock_free_all() completed:                          │
│    ✓ All unreserved RAM freed to buddy                   │
│    ✓ Buddy merging consolidated max-order blocks         │
│    ✓ zone->managed_pages accurate                        │
│    ✓ totalram_pages set                                  │
│    ✓ after_bootmem = 1                                   │
│                                                          │
│  Page states:                                            │
│    Free pages: _refcount=0, PG_reserved=0, on free list  │
│    Reserved:   _refcount=1, PG_reserved=1, NOT on list   │
│    Kernel image: reserved (never freed to buddy)          │
│    memblock metadata: freed (or discarded later)          │
│                                                          │
│  Allocation API now available:                           │
│    alloc_pages(gfp, order) → buddy allocator             │
│    __get_free_pages(gfp, order) → returns VA             │
│    free_pages(addr, order)                               │
│                                                          │
│  NOT YET:                                                │
│    ✗ SLUB slab allocator (kmalloc, kmem_cache_alloc)     │
│    ✗ vmalloc (kernel virtual allocations)                │
│                                                          │
│  Next: SLUB bootstrap (kmem_cache_init)                  │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 06: Zone, Node & Page Init](06_Zone_Node_Page_Init.md) | **Doc 07** | [Doc 08: SLUB Bootstrap →](08_SLUB_Bootstrap_Code.md)
