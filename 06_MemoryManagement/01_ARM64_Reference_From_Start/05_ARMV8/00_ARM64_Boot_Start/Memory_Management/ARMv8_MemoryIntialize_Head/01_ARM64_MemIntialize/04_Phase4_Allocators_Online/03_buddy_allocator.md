# Buddy Allocator — The Page-Level Memory Manager

**Source:** `mm/page_alloc.c`
**Phase:** Active after `memblock_free_all()` — kernel's permanent page allocator
**Purpose:** Allocate and free physical pages in power-of-2 blocks (orders 0-10)

---

## What the Buddy Allocator Is

The buddy allocator is the kernel's **primary physical page allocator**. Every physical page allocation goes through it (directly or indirectly). It manages free pages in a per-zone array of free lists, organized by order (power-of-2 page counts).

```
Order 0:  1 page   =   4 KB
Order 1:  2 pages  =   8 KB
Order 2:  4 pages  =  16 KB
Order 3:  8 pages  =  32 KB
Order 4:  16 pages =  64 KB
Order 5:  32 pages = 128 KB
Order 6:  64 pages = 256 KB
Order 7:  128 pages = 512 KB
Order 8:  256 pages = 1 MB
Order 9:  512 pages = 2 MB
Order 10: 1024 pages = 4 MB (MAX_PAGE_ORDER)
```

---

## Allocation: `__alloc_pages()` — The Heart of Page Allocation

```c
struct page *__alloc_pages(gfp_t gfp, unsigned int order,
                           int preferred_nid, nodemask_t *nodemask)
```

### The Fast Path

```c
// Simplified __alloc_pages flow:

// 1. Prepare allocation context
ac.highest_zoneidx = gfp_zone(gfp);    // Target zone (DMA, DMA32, NORMAL)
ac.preferred_zoneref = first_zones_zonelist(zonelist, ac.highest_zoneidx);

// 2. FAST PATH — Try per-CPU cache and buddy free lists
page = get_page_from_freelist(gfp, order, alloc_flags, &ac);
if (page)
    return page;   // Success! Most allocations succeed here.

// 3. SLOW PATH — Memory pressure, try harder
page = __alloc_pages_slowpath(gfp, order, &ac);
return page;
```

### `get_page_from_freelist()` — Zone Scanning

```c
static struct page *get_page_from_freelist(gfp_t gfp, unsigned int order,
                                            int alloc_flags,
                                            const struct alloc_context *ac)
{
    // Walk the zonelist (preferred zone first, then fallbacks)
    for_each_zone_zonelist(zone, z, ac->zonelist, ac->highest_zoneidx) {

        // Check watermarks
        if (!zone_watermark_fast(zone, order, mark,
                                  ac->highest_zoneidx, alloc_flags))
            continue;   // Not enough free pages, try next zone

        // Try to allocate from this zone
        page = rmqueue(ac->preferred_zoneref->zone, zone, order,
                       gfp, alloc_flags, ac->migratetype);
        if (page)
            return page;
    }
    return NULL;  // All zones exhausted
}
```

### `rmqueue()` — Get Pages from Zone

```c
static struct page *rmqueue(struct zone *preferred_zone, struct zone *zone,
                             unsigned int order, gfp_t gfp_flags,
                             unsigned int alloc_flags, int migratetype)
{
    if (likely(order == 0)) {
        // ORDER-0: Try per-CPU cache first (no lock needed!)
        page = rmqueue_pcplist(preferred_zone, zone, order,
                               migratetype, alloc_flags);
        if (page)
            return page;
    }

    // Higher orders or PCP miss: go to buddy free lists
    spin_lock_irqsave(&zone->lock, flags);
    page = __rmqueue(zone, order, migratetype, alloc_flags);
    spin_unlock_irqrestore(&zone->lock, flags);

    return page;
}
```

### Per-CPU Page Cache (PCP) — Order-0 Fast Path

```
rmqueue_pcplist():
  1. Get this CPU's per_cpu_pages for the zone
  2. Look for a free page in pcp->lists[migratetype]
  3. If found: unlink and return (NO ZONE LOCK!)
  4. If empty: take zone->lock, move 'batch' pages from zone to PCP

Per-CPU cache avoids zone lock contention:
  CPU 0 allocates from its own cache → no lock
  CPU 1 allocates from its own cache → no lock
  CPU 2 allocates from its own cache → no lock
  Only when PCP runs dry does any CPU take zone->lock
```

### `__rmqueue()` — Buddy Free List Walk

```c
static struct page *__rmqueue(struct zone *zone, unsigned int order,
                               int migratetype, unsigned int alloc_flags)
{
    page = __rmqueue_smallest(zone, order, migratetype);
    if (page)
        return page;

    // Try to steal from other migration types
    page = __rmqueue_fallback(zone, order, migratetype, alloc_flags);
    return page;
}
```

### `__rmqueue_smallest()` — Find Best-Fit Block

```c
static struct page *__rmqueue_smallest(struct zone *zone,
                                        unsigned int order,
                                        int migratetype)
{
    // Walk free lists from requested order UP to MAX_PAGE_ORDER
    for (current_order = order; current_order < NR_PAGE_ORDERS;
         ++current_order) {

        area = &(zone->free_area[current_order]);
        page = get_page_from_free_area(area, migratetype);

        if (!page)
            continue;  // No free block at this order

        // Found a free block!
        del_page_from_free_list(page, zone, current_order);

        // Split if block is larger than requested
        expand(zone, page, order, current_order, migratetype);

        return page;
    }
    return NULL;  // No block found at any order
}
```

### `expand()` — Block Splitting

When a larger block is found, it's split into two buddies repeatedly until the requested order is reached:

```
Request: order-2 (4 pages)
Found:   order-4 (16 pages) at PFN 0x40000

expand(zone, page, order=2, current_order=4):

Step 1: Split order-4 into two order-3 blocks
  [PFN 0x40000, order-3] ← Keep (still too large)
  [PFN 0x40008, order-3] → Add to free_area[3].free_list[migratetype]

Step 2: Split order-3 into two order-2 blocks
  [PFN 0x40000, order-2] ← Return to caller (correct size!)
  [PFN 0x40004, order-2] → Add to free_area[2].free_list[migratetype]

Result:
  Before: free_area[4] has 1 block (16 pages)
  After:  free_area[4] has 0 blocks
          free_area[3] has 1 block (8 pages)
          free_area[2] has 1 block (4 pages)
          Returned: 1 block (4 pages) to caller
```

```
Visual:
Before: [████████████████]  16 pages (order 4)

Split:  [████████][████████]  2 × 8 pages (order 3)
         keep      → free_area[3]

Split:  [████][████][████████]  4+4+8 pages
         return  → free_area[2]  (already free)
```

---

## Free: `__free_pages()` — Returning Pages to Buddy

```c
void __free_pages(struct page *page, unsigned int order)
{
    if (put_page_testzero(page)) {   // Decrement refcount, check if zero
        free_the_page(page, order);
    }
}

static void free_the_page(struct page *page, unsigned int order)
{
    if (pcp_allowed_order(order)) {
        // Order 0-3: Return to per-CPU cache
        free_unref_page(page, order);
    } else {
        // Order 4+: Return directly to buddy
        __free_pages_ok(page, order, FPI_NONE);
    }
}
```

### `__free_one_page()` — Buddy Merging

```c
static void __free_one_page(struct page *page, unsigned long pfn,
                             struct zone *zone, unsigned int order,
                             int migratetype, fpi_t fpi_flags)
{
    unsigned long buddy_pfn;
    unsigned long combined_pfn;
    struct page *buddy;

    // Try to merge with buddy at each order level
    while (order < MAX_PAGE_ORDER) {
        // Find buddy PFN using XOR trick
        buddy_pfn = pfn ^ (1 << order);
        buddy = page + (buddy_pfn - pfn);

        // Check if buddy is free and at the same order
        if (!page_is_buddy(page, buddy, order))
            break;   // Buddy is not free — can't merge

        // Buddy is free! Merge.
        del_page_from_free_list(buddy, zone, order);  // Remove buddy from its list

        // Determine combined block's PFN (lower of the two)
        combined_pfn = pfn & ~(1UL << order);
        page = pfn_to_page(combined_pfn);
        pfn = combined_pfn;

        order++;   // Merged block is one order higher
    }

    // Add the (possibly merged) block to the appropriate free list
    add_to_free_list(page, zone, order, migratetype);
}
```

### The XOR Buddy Trick

```
Buddy PFN = PFN ^ (1 << order)

This works because buddy blocks are always aligned to their order:

Order 0: Buddies differ in bit 0
  PFN 0b...0100 ↔ PFN 0b...0101   (XOR with 1)

Order 1: Buddies differ in bit 1
  PFN 0b...0100 ↔ PFN 0b...0110   (XOR with 2)

Order 2: Buddies differ in bit 2
  PFN 0b...0100 ↔ PFN 0b...0000   (XOR with 4)

Two blocks can merge only if:
  1. Both are free
  2. Both are at the same order
  3. Their PFNs differ only in the bit corresponding to their order
```

### Merging Example

```
Free page at PFN 0x40005, order 0:

Step 1: buddy_pfn = 0x40005 ^ 1 = 0x40004
  Is page[0x40004] free at order 0? YES!
  Merge → combined PFN = 0x40004, order 1

Step 2: buddy_pfn = 0x40004 ^ 2 = 0x40006
  Is page[0x40006] free at order 1? YES!
  Merge → combined PFN = 0x40004, order 2

Step 3: buddy_pfn = 0x40004 ^ 4 = 0x40000
  Is page[0x40000] free at order 2? NO (still allocated)
  Stop merging.

Result: Add block at PFN 0x40004, order 2 to free_area[2]
```

---

## Migration Type Fallback

When the preferred migration type has no free blocks, the allocator steals from other types:

```c
static int fallbacks[MIGRATE_TYPES][MIGRATE_TYPES - 1] = {
    [MIGRATE_UNMOVABLE]  = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE   },
    [MIGRATE_MOVABLE]    = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE },
    [MIGRATE_RECLAIMABLE]= { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE  },
};
```

When stealing, the allocator tries to **steal the entire pageblock** (2MB) to keep migration types grouped:

```
Request: MIGRATE_UNMOVABLE, order 0
free_area[0].free_list[UNMOVABLE] = empty

Fallback:
  1. Check free_area[0].free_list[RECLAIMABLE]
  2. Check free_area[0].free_list[MOVABLE]

  If stealing at high order: change entire pageblock's migration type
  This prevents fragmentation of the pageblock
```

---

## The Slow Path: `__alloc_pages_slowpath()`

When the fast path fails (no pages at required watermark level):

```
__alloc_pages_slowpath():
  1. Wake kswapd (background page reclaim)
  2. Retry allocation with lowered watermarks
  3. If still fails: try direct reclaim
     - Scan LRU lists, write back dirty pages
     - Free page cache pages
     - Retry allocation
  4. If still fails: try compaction
     - Defragment by migrating movable pages
     - Creates larger contiguous blocks
     - Retry allocation
  5. If still fails and GFP_NOFAIL: loop forever
  6. Otherwise: return NULL (allocation failed)

  For __GFP_NORETRY: skip most retries
  For __GFP_NOFAIL:  never return NULL (loops forever)
```

---

## Summary of Allocation/Free Flow

```
ALLOCATION:
  __alloc_pages(gfp, order)
  ├── Fast path: get_page_from_freelist()
  │   ├── Zone walk (zonelist order)
  │   ├── Watermark check
  │   ├── rmqueue()
  │   │   ├── PCP cache (order 0, no lock)         ← FASTEST
  │   │   └── __rmqueue_smallest() + expand()       ← Takes zone lock
  │   └── Return page
  └── Slow path: __alloc_pages_slowpath()
      ├── Wake kswapd
      ├── Direct reclaim
      ├── Compaction
      └── OOM killer (last resort)

FREE:
  __free_pages(page, order)
  ├── PCP cache (order 0-3, minimal locking)        ← FASTEST
  └── __free_one_page() + buddy merging              ← Takes zone lock
```

---

## Key Takeaways

1. **Power-of-2 only** — the buddy system only handles 4KB–4MB allocations; smaller objects use SLUB
2. **XOR is the magic** — finding a buddy is a single XOR operation on the PFN
3. **Per-CPU caches eliminate contention** — most order-0 allocations never take a lock
4. **Splitting and merging are symmetric** — allocation splits down, freeing merges up
5. **Migration types fight fragmentation** — grouping movable/unmovable pages prevents intermixing
6. **The slow path has many fallbacks** — kswapd, direct reclaim, compaction, OOM killer
7. **GFP flags control everything** — `GFP_KERNEL`, `GFP_ATOMIC`, `GFP_DMA` determine zone, sleeping, reclaim behavior
