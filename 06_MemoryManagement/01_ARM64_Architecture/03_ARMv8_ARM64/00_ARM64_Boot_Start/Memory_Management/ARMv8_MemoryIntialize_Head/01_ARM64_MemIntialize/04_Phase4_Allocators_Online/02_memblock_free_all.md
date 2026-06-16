# `memblock_free_all()` — The Great Transition to Buddy

**Source:** `mm/memblock.c` lines 2340–2380
**Phase:** Allocators Online — THE transition point
**Memory Allocator:** Memblock → Buddy (this function bridges them)
**Called by:** `mm_core_init()`
**Calls:** `free_low_memory_core_early()`, `__free_pages_core()`, `totalram_pages_add()`

---

## What This Function Does

This is the **single most important function** in the memblock→buddy transition. It iterates all free (non-reserved) memblock memory and hands each page to the buddy allocator.

After this function returns:
- Memblock is effectively retired
- The buddy allocator owns all free memory
- `alloc_pages()` works

---

## How It Works With Memory

### The Transition

```
BEFORE:
  memblock.memory    = [all RAM regions]
  memblock.reserved  = [kernel, DTB, page tables, struct page[], etc.]
  buddy free_area[]  = EMPTY (no pages)
  zone->managed_pages = 0

AFTER:
  memblock           = RETIRED (data still in .init, code will be freed)
  buddy free_area[]  = POPULATED with all free pages
  zone->managed_pages = actual count of freed pages
```

---

## Step-by-Step Execution

### Step 1: Reset Zone Managed Pages

```c
void __init memblock_free_all(void)
{
    unsigned long pages;

    reset_all_zones_managed_pages();
```

Sets `zone->managed_pages = 0` for every zone. As pages are freed to buddy, `managed_pages` is incremented.

---

### Step 2: Free All Non-Reserved Memory

```c
    pages = free_low_memory_core_early();
```

This is the core — iterates all free memblock ranges and frees them.

### `free_low_memory_core_early()` — Iterating Free Ranges

```c
static unsigned long __init free_low_memory_core_early(void)
{
    unsigned long count = 0;
    phys_addr_t start, end;
    u64 i;

    memblock_clear_hotplug(0, -1);  // Clear hotplug flags

    // Iterate all FREE memory (memory minus reserved)
    for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE,
                            &start, &end, NULL) {
        count += __free_memory_core(start, end);
    }

    return count;
}
```

**`for_each_free_mem_range()`** computes free ranges on-the-fly:

```
memblock.memory:   [████████████████████████████████████████████]
memblock.reserved: [  ████  ] [████████]   [████]  [████████]

Free ranges:       [██      ██          ███      ██          ██]
                    ↑        ↑           ↑        ↑           ↑
                    Range 1  Range 2     Range 3  Range 4     Range 5
```

Each free range is passed to `__free_memory_core()`.

---

### Step 3: `__free_memory_core()` — Free a Range of Pages

```c
static unsigned long __init __free_memory_core(phys_addr_t start,
                                                phys_addr_t end)
{
    unsigned long start_pfn = PFN_UP(start);   // Round up (don't free partial pages)
    unsigned long end_pfn = PFN_DOWN(end);     // Round down
    unsigned long count = 0;

    // Free pages in maximum buddy-order chunks
    while (start_pfn < end_pfn) {
        int order = min_t(int, MAX_PAGE_ORDER,
                          __ffs(start_pfn));   // Alignment-limited order

        while (start_pfn + (1UL << order) > end_pfn)
            order--;  // Size-limited order

        __free_pages_core(pfn_to_page(start_pfn), order);

        count += 1UL << order;
        start_pfn += 1UL << order;
    }

    return count;
}
```

**Algorithm: Free pages in the largest possible buddy blocks.**

```
Example: Free range [PFN 0x40000, PFN 0x80000) = 256K pages = 1 GB

Iteration 1: start_pfn = 0x40000
  __ffs(0x40000) = 18, limited to MAX_PAGE_ORDER = 10
  Free order-10 block (1024 pages = 4 MB) at PFN 0x40000

Iteration 2: start_pfn = 0x40400
  Free order-10 block at PFN 0x40400

... (256 iterations to free 1 GB in 4MB chunks)
```

---

### Step 4: `__free_pages_core()` — Free One Buddy Block

```c
void __init __free_pages_core(struct page *page, unsigned int order)
{
    unsigned int nr_pages = 1 << order;
    struct page *p = page;
    unsigned int loop;

    // Step A: Prepare each page in the block
    prefetchw(p);
    for (loop = 0; loop < (nr_pages - 1); loop++, p++) {
        prefetchw(p + 1);
        __ClearPageReserved(p);      // Clear PageReserved flag
        set_page_count(p, 0);        // Set refcount to 0 (free)
    }
    __ClearPageReserved(p);
    set_page_count(p, 0);

    // Step B: Adjust zone managed_pages
    atomic_long_add(nr_pages, &page_zone(page)->managed_pages);

    // Step C: Set the buddy order
    set_page_order(page, order);

    // Step D: FREE TO BUDDY
    __free_pages_ok(page, order, FPI_TO_TAIL);
}
```

### Step A: Clear PageReserved

Every `struct page` was initialized with `PageReserved` in Phase 3. This flag means "do not touch — this page is not managed by the buddy allocator." Clearing it transitions the page to buddy management.

### Step B: Update managed_pages

`zone->managed_pages` tracks how many pages the buddy allocator manages. This counter drives watermark calculations.

### Step C: Set Buddy Order

For the **head page** of a free block, `page->private` stores the buddy order:

```
Order 10 block (1024 pages):
  page[0].private = 10    ← Head page, stores order
  page[1].private = 0     ← Tail page
  ...
  page[1023].private = 0  ← Tail page
```

### Step D: Add to Buddy Free List

```c
__free_pages_ok(page, order, FPI_TO_TAIL):
  1. Determine zone from page->flags
  2. Determine migration type from pageblock bitmap
  3. Try to merge with buddy:
     __free_one_page(page, pfn, zone, order, migratetype, FPI_TO_TAIL)
```

**FPI_TO_TAIL** — adds to the **tail** of the free list (cold end), not the head. During boot, we're freeing many pages sequentially, and adding to tail prevents cache-hot pages from being pushed out.

---

## Buddy Merging During Free

When `__free_one_page()` receives a block, it tries to merge with its buddy:

```
__free_one_page(page, pfn=0x40000, zone, order=0):

  Buddy PFN = 0x40000 ^ (1 << 0) = 0x40001
  Is buddy free at order 0? Check page[0x40001].flags

  If YES:
    Remove buddy from free_list[order=0]
    Merge: new block at PFN 0x40000, order=1

    Buddy PFN = 0x40000 ^ (1 << 1) = 0x40002
    Is buddy free at order 1?

    If YES:
      Merge again: order=2
      Continue merging...

    If NO:
      Add merged block to free_list[order=1]

  If NO:
    Add page to free_list[order=0]
```

**During `memblock_free_all()`, blocks are freed at the maximum order**, so merging is minimal — the blocks are already large.

---

### Step 5: Update Total RAM Counter

```c
    totalram_pages_add(pages);
}
```

Updates the global `_totalram_pages` counter — used by `/proc/meminfo`, `sysinfo()`, and OOM calculations.

---

## Page State Transitions During This Function

```
BEFORE (Phase 3 initialization):
  page->flags: PG_reserved SET
  page->_refcount: 1
  page->lru: empty
  State: RESERVED (not managed by buddy)

AFTER __free_pages_core():
  page->flags: PG_reserved CLEARED, PG_buddy SET (for head page)
  page->_refcount: 0
  page->lru: linked into zone->free_area[order].free_list[migratetype]
  page->private: order (for head page)
  State: FREE (in buddy free list)
```

---

## Memory Accounting After memblock_free_all()

```
Example: 4 GB system

memblock.memory total = 4,194,304 KB
memblock.reserved:
  - Kernel image:   32,768 KB
  - DTB:               64 KB
  - Page tables:      256 KB
  - struct page[]:  65,536 KB
  - initrd:        16,384 KB
  - CMA:           65,536 KB (freed to buddy as MIGRATE_CMA)
  - Other:          5,000 KB

Freed to buddy ≈ 4,194,304 - 120,000 ≈ 4,074,304 KB
                ≈ 3,978 MB ≈ 97% of total RAM

dmesg: "Memory: 3978096K/4194304K available"
```

---

## Key Takeaways

1. **This is THE transition** — before: memblock only; after: buddy only
2. **Pages are freed in max-order chunks** — efficient (fewer free operations, less merging)
3. **PageReserved → cleared** — the flag flip that makes pages buddy-managed
4. **managed_pages accumulates** — each zone counts its freed pages
5. **CMA pages are freed too** — but as MIGRATE_CMA type (can be reclaimed for contiguous allocation)
6. **FPI_TO_TAIL ordering** — boot-time frees go to list tail, preserving cache warmth
7. **After this, memblock is dead** — its `.init` code/data will be freed when `free_initmem()` runs
