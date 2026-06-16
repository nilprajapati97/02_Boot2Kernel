# Layer 8 — The Great Handoff: memblock → Buddy Allocator

[← Layer 7: Zones, Nodes & Page Init](07_Zones_Nodes_and_Page_Init.md) | **Layer 8** | [Layer 9: Slab, vmalloc & Runtime →](09_Slab_Vmalloc_and_Runtime.md)

---

## What This Layer Covers

All the infrastructure is in place — zones, nodes, struct page arrays, empty buddy free
lists. Now comes the most critical transition in the entire boot sequence:
**`memblock_free_all()`** takes every unreserved physical page and feeds it into the
buddy allocator's free lists. After this layer, `alloc_pages()` works and the kernel
has a fully functional page allocator.

---

## 1. `mm_core_init()` — The Orchestrator

**Source**: `mm/mm_init.c`

Back in `start_kernel()`, after `mm_core_init_early()` (which ran `free_area_init`),
the next major call is `mm_core_init()`:

```
mm_core_init()                                    [mm/mm_init.c]
    │
    ├──► build_all_zonelists(NULL)
    │       Build the fallback zone list for each node.
    │       Determines the order in which zones are tried
    │       when allocating pages:
    │
    │       Typical fallback for GFP_KERNEL allocation:
    │         ZONE_NORMAL → ZONE_DMA32 → ZONE_DMA
    │       (Try Normal first; if empty, fall back to DMA32, then DMA)
    │
    ├──► page_alloc_init_cpuhp()       CPU hotplug callbacks
    ├──► page_ext_init_flatmem()       Page extension (debug)
    ├──► mem_debugging_and_hardening_init()
    ├──► kfence_alloc_pool_and_metadata()
    ├──► stack_depot_early_init()
    │
    ├──► memblock_free_all()  ★★★  THE GREAT HANDOFF  ★★★
    │       Release all unreserved memblock memory to buddy.
    │       (Explained in detail in Section 2)
    │
    ├──► mem_init()                    Arch-specific finalization
    │                                  (mostly empty on ARM64)
    │
    ├──► kmem_cache_init()  ★          SLUB bootstrap (Layer 9)
    │
    ├──► vmalloc_init()     ★          vmalloc subsystem (Layer 9)
    │
    └──► (other initializations)
```

---

## 2. `memblock_free_all()` — The Critical Transition

**Source**: `mm/memblock.c`

This single function call transforms the memory subsystem from "memblock-based early
allocator" to "buddy-based runtime allocator":

```
memblock_free_all()
    │
    ├──► free_unused_memmap()
    │       Free struct page descriptors for physical holes
    │       (memory gaps between memblock regions).
    │
    ├──► reset_all_zones_managed_pages()
    │       Set managed_pages = 0 for EVERY zone.
    │       Why? Because managed_pages will be rebuilt from scratch
    │       as each page is freed to the buddy system.
    │
    ├──► pages = free_low_memory_core_early()     ★
    │       THE ACTUAL HANDOFF — iterate all unreserved memblock
    │       regions and free each page to the buddy allocator.
    │       Returns the total number of pages freed.
    │       (Explained in detail in Section 3)
    │
    └──► totalram_pages_add(pages)
            Set the global totalram_pages counter.
            This is the number the kernel reports as "total memory".
```

---

## 3. `free_low_memory_core_early()` — Walking the Free Regions

**Source**: `mm/memblock.c`

```
free_low_memory_core_early()
    │
    ├──► memmap_init_reserved_pages()
    │       Walk ALL memblock.reserved regions.
    │       For each reserved page: set PG_Reserved in its struct page.
    │       Also set the NUMA node ID in each reserved page.
    │       This ensures reserved pages are NEVER freed to buddy.
    │
    └──► Iterate all FREE memblock regions:
         │
         │   "Free" = in memblock.memory BUT NOT in memblock.reserved
         │
         │   for_each_free_mem_range(i, NUMA_NO_NODE, ..., &start, &end, ...):
         │
         │   ┌─────────────────────────────────────────────────────┐
         │   │  memblock.memory:    [=====A=====]  [====B====]     │
         │   │  memblock.reserved:       [rr]            [r]       │
         │   │                      ─────────────────────────────   │
         │   │  Free ranges:        [===]    [==]  [====]  [=]     │
         │   │                                                     │
         │   │  Each free range is processed:                      │
         │   └─────────────────────────────────────────────────────┘
         │
         └──► For each free range [start, end):
              │
              └── __free_memory_core(start, end)
                  └── __free_pages_memory(start_pfn, nr_pages)
                      │
                      │   Break the range into largest power-of-2
                      │   aligned chunks:
                      │
                      │   While pages remain:
                      │     order = min(MAX_PAGE_ORDER,
                      │                 __ffs(pfn),        // alignment
                      │                 ilog2(nr_pages))   // remaining
                      │
                      │     memblock_free_pages(pfn, order)
                      │     pfn += (1 << order)
                      │     nr_pages -= (1 << order)
                      │
                      │   Example: 5 pages starting at PFN 0x40000:
                      │     order=2 (4 pages at 0x40000)  → free 4
                      │     order=0 (1 page  at 0x40004)  → free 1
                      │
                      └──────────────────────────────────────
```

---

## 4. `memblock_free_pages()` → `__free_pages_core()` — Into the Buddy System

**Source**: `mm/mm_init.c`, `mm/page_alloc.c`

This is where individual page blocks actually enter the buddy allocator:

```
memblock_free_pages(pfn, order)                   [mm/mm_init.c]
    │
    └──► __free_pages_core(page, order, MEMINIT_EARLY)    [mm/page_alloc.c]
         │
         │   For each page in the block (2^order pages):
         │
         ├──► 1. Clear PG_Reserved
         │       __ClearPageReserved(page + i)
         │       The page is no longer "reserved" — it can be allocated.
         │
         ├──► 2. Set page count to 0
         │       set_page_count(page + i, 0)
         │       refcount 0 = free page (was 1 = reserved at boot)
         │
         └──► 3. Increment zone->managed_pages
              │   atomic_long_add(1 << order, &zone->managed_pages)
              │   This rebuilds the managed_pages counter accurately.
              │
              └── 4. __free_pages_ok(page, order, FPI_TO_TAIL)
                  └── free_one_page(zone, page, pfn, order, migratetype)
                      └── __free_one_page(page, pfn, zone, order, mt)
                              ★ THE BUDDY ALGORITHM ★
                              (Explained in Section 5)
```

---

## 5. `__free_one_page()` — The Buddy Merging Algorithm

**Source**: `mm/page_alloc.c`

This is the heart of the buddy allocator — the algorithm that merges adjacent free blocks
into larger blocks:

### 5.1 The Buddy Concept

```
The Buddy System Concept:
═════════════════════════

Physical pages are grouped into power-of-2 blocks:
  Order 0:  1 page   (4 KB)
  Order 1:  2 pages  (8 KB)
  Order 2:  4 pages  (16 KB)
  ...
  Order 10: 1024 pages (4 MB) = MAX_PAGE_ORDER

Two adjacent blocks of the same order are "buddies":

  PFN:  0    1    2    3    4    5    6    7
       ├────┤    ├────┤    ├────┤    ├────┤    Order 0 buddies
       ├─────────┤    ├─────────┤              Order 1 buddies
       ├──────────────────────┤                Order 2 buddies
       ├──────────────────────────────────┤    Order 3 buddy (8 pages)

Finding your buddy:
  buddy_pfn = pfn XOR (1 << order)

  Example: PFN 4, order 1 (2-page block at PFN 4-5)
    buddy = 4 XOR (1 << 1) = 4 XOR 2 = 6
    → Buddy is the 2-page block at PFN 6-7
```

### 5.2 The Merging Algorithm

```
__free_one_page(page, pfn, zone, order, migratetype)
    │
    │   Starting at the given order, try to merge with buddy:
    │
    │   while (order < MAX_PAGE_ORDER):
    │   │
    │   ├── buddy_pfn = pfn ^ (1 << order)
    │   │       Find the buddy's PFN using XOR.
    │   │
    │   ├── buddy = pfn_to_page(buddy_pfn)
    │   │       Get the buddy's struct page.
    │   │
    │   ├── Is buddy free AND same order?
    │   │   │
    │   │   ├── NO: Stop merging. Add current block to freelist.
    │   │   │
    │   │   └── YES:
    │   │       ├── Remove buddy from its free_list
    │   │       │   del_page_from_free_list(buddy, zone, order)
    │   │       │
    │   │       ├── combined_pfn = pfn & ~(1 << order)
    │   │       │   The combined block starts at the lower PFN.
    │   │       │
    │   │       ├── page = pfn_to_page(combined_pfn)
    │   │       ├── pfn = combined_pfn
    │   │       └── order++
    │   │           Try to merge at the next higher order.
    │   │
    │   └── (continue while loop)
    │
    └── Add the (possibly merged) block to the free list:
        set_buddy_order(page, order)
        add_to_free_list(page, zone, order, migratetype)
        zone->free_area[order].nr_free++
```

### 5.3 Merging Example

```
Freeing PFN 6 (order 0):

Step 1: order=0, pfn=6
        buddy = 6 XOR 1 = 7
        Is page 7 free at order 0? YES!
        → Remove page 7 from free_area[0]
        → combined_pfn = 6 & ~1 = 6, order becomes 1

Step 2: order=1, pfn=6
        buddy = 6 XOR 2 = 4
        Is page 4 free at order 1? YES!
        → Remove page 4 from free_area[1]
        → combined_pfn = 4 & ~2 = 4, order becomes 2

Step 3: order=2, pfn=4
        buddy = 4 XOR 4 = 0
        Is page 0 free at order 2? NO (it's reserved)
        → STOP merging

Result: Add PFN 4, order 2 (4-page block) to free_area[2]

Before:  free_area[0]: {7}    free_area[1]: {4}
After:   free_area[0]: {}     free_area[1]: {}     free_area[2]: {4}

Four separate pages merged into one order-2 block!
```

---

## 6. The Free List Structure

After `memblock_free_all()` completes, the buddy free lists look like this:

```
zone->free_area[] (for each zone)
═════════════════════════════════

free_area[0]  (order 0: 4 KB blocks)
├── free_list[MIGRATE_UNMOVABLE]  ──► page ──► page ──► page ──► ...
├── free_list[MIGRATE_MOVABLE]    ──► page ──► page ──► ...
├── free_list[MIGRATE_RECLAIMABLE] ──► (usually empty at boot)
├── free_list[MIGRATE_HIGHATOMIC]  ──► (empty)
└── nr_free = count of all order-0 blocks

free_area[1]  (order 1: 8 KB blocks)
├── free_list[MIGRATE_UNMOVABLE]  ──► page ──► page ──► ...
├── free_list[MIGRATE_MOVABLE]    ──► page ──► page ──► ...
├── ...
└── nr_free = count of all order-1 blocks

...

free_area[10]  (order 10: 4 MB blocks)
├── free_list[MIGRATE_UNMOVABLE]  ──► page ──► ...
├── free_list[MIGRATE_MOVABLE]    ──► page ──► page ──► ...
├── ...
└── nr_free = count of all order-10 blocks


After boot, most free memory ends up in HIGH ORDER free lists
(order 9-10) because large contiguous regions merge together.
```

### 6.1 Migration Types

```
MIGRATE_UNMOVABLE    Pages that cannot be moved (kernel data structures,
                     page tables, slab caches). Allocated with GFP_KERNEL.

MIGRATE_MOVABLE      Pages that can be moved/migrated (user pages, page
                     cache). Allocated with GFP_HIGHUSER_MOVABLE.
                     Enables memory compaction and hotplug.

MIGRATE_RECLAIMABLE  Pages that can be reclaimed (page cache, dentries).
                     Allocated with GFP_KERNEL|__GFP_RECLAIMABLE.

MIGRATE_HIGHATOMIC   Reserve for high-priority atomic allocations.

MIGRATE_CMA          Contiguous Memory Allocator region.
                     Can be used for movable allocations when CMA
                     doesn't need it.
```

---

## 7. `alloc_pages()` — How Runtime Page Allocation Works

Now that the buddy allocator has pages, here's how `alloc_pages()` works:

```
alloc_pages(gfp_flags, order)
    │
    ├──► Determine zone preference from gfp_flags:
    │       GFP_KERNEL     → prefer ZONE_NORMAL
    │       GFP_DMA32      → prefer ZONE_DMA32
    │       GFP_DMA        → prefer ZONE_DMA
    │       GFP_HIGHUSER   → prefer ZONE_NORMAL (movable)
    │
    ├──► get_page_from_freelist(gfp, order, alloc_flags, ac)
    │    │
    │    │   For each zone in the fallback zonelist:
    │    │
    │    ├── Check zone watermarks:
    │    │     Is free_pages >= watermark[WMARK_LOW]?
    │    │     ├── YES: proceed to allocate
    │    │     └── NO: try next zone, or trigger reclaim
    │    │
    │    └── rmqueue(zone, order, migratetype)
    │         │
    │         ├── Order 0 (single page): Try PER-CPU CACHE first
    │         │    │
    │         │    └── rmqueue_pcplist(zone, migratetype)
    │         │        PCP = zone->per_cpu_pageset (per-CPU)
    │         │        If PCP list has pages → return one (FAST PATH)
    │         │        If empty → refill from buddy free list
    │         │
    │         └── Order > 0: Go directly to buddy free list
    │              │
    │              └── __rmqueue(zone, order, migratetype)
    │                   │
    │                   │   Try to find a free block at the requested order:
    │                   │
    │                   ├── Look in free_area[order].free_list[migratetype]
    │                   │   Found? Remove from list → return
    │                   │
    │                   ├── If empty, try HIGHER orders:
    │                   │   free_area[order+1], free_area[order+2], ...
    │                   │   Found at higher order?
    │                   │   → Split: take one half, return other half
    │                   │     to lower free_area[].
    │                   │
    │                   └── If still empty, try other migration types:
    │                       "Page stealing" — take from MOVABLE freelist
    │                       even though we wanted UNMOVABLE. Change the
    │                       pageblock's migratetype if stealing enough.
    │
    └──► If no page found:
         → Direct reclaim (shrink caches, swap out pages)
         → Compaction (move pages to create contiguous blocks)
         → OOM killer (kill a process to free memory)
         → Retry allocation
```

### 7.1 Per-CPU Page Caches (PCP)

Single-page allocations (order-0) are the most common. To avoid the zone lock overhead,
each CPU has its own page cache:

```
Per-CPU Page Cache (struct per_cpu_pages)
═════════════════════════════════════════

Each CPU has a local cache of free pages per migratetype:

CPU 0:  PCP[MOVABLE]    → page, page, page, page, ... (up to 'high')
        PCP[UNMOVABLE]  → page, page, ...
        PCP[RECLAIMABLE]→ ...

CPU 1:  PCP[MOVABLE]    → page, page, ...
        ...

Allocation: take from PCP (no lock needed — per-CPU)
Deallocation: put on PCP
When PCP is empty: refill 'batch' pages from buddy (take zone lock)
When PCP is full: drain 'batch' pages back to buddy (take zone lock)

batch   = typically 31 pages
high    = typically 186 pages (6 × batch)
```

---

## 8. Zone Watermarks

```
Zone Watermarks (per zone)
══════════════════════════

                              ┌─────────────────────┐
      Total zone pages ────── │                     │
                              │   Freely allocatable │
                              │                     │
 watermark[WMARK_HIGH] ────── ├─────────────────────┤
                              │   kswapd stops here  │
                              │                     │
 watermark[WMARK_LOW] ─────── ├─────────────────────┤
                              │   kswapd starts here │
                              │   (background reclaim)│
                              │                     │
 watermark[WMARK_MIN] ─────── ├─────────────────────┤
                              │   Direct reclaim     │
                              │   (caller blocks)    │
                              │                     │
           0 ──────────────── └─────────────────────┘

If free pages drop below WMARK_LOW:
  → kswapd daemon wakes up and reclaims pages in the background

If free pages drop below WMARK_MIN:
  → Allocating thread must do direct reclaim (blocks until pages freed)

If free pages are above WMARK_HIGH:
  → kswapd goes back to sleep
```

---

## 9. The dmesg Output

After `memblock_free_all()`, the kernel prints its famous memory summary:

```
[    0.000000] Memory: 5765432K/6291456K available (12345K kernel code,
               1234K rwdata, 5678K rodata, 2345K init, 567K bss,
               525024K reserved, 0K cma-reserved)
```

Where:
- **6291456K** = total physical RAM (6 GB)
- **5765432K** = pages freed to buddy (managed_pages total)
- **525024K** = reserved (kernel, FDT, initrd, page tables, struct page arrays, etc.)

---

## 10. State After Layer 8

```
┌────────────────────────────────────────────────────────────────┐
│              STATE AFTER memblock_free_all()                    │
│                                                                │
│  Buddy Allocator:                                              │
│    ★ FULLY OPERATIONAL ★                                       │
│    ✓ All unreserved pages in buddy free lists                  │
│    ✓ Pages merged into highest possible orders                 │
│    ✓ alloc_pages() works for any order                         │
│    ✓ Per-CPU page caches initialized (but may be empty)        │
│    ✓ Zone watermarks will be computed shortly                  │
│                                                                │
│  Memory Accounting:                                            │
│    ✓ totalram_pages set                                        │
│    ✓ zone->managed_pages accurate for each zone                │
│    ✓ Reserved pages have PG_Reserved set                       │
│                                                                │
│  What Still Doesn't Work:                                      │
│    ✗ kmalloc() — needs slab allocator (Layer 9)                │
│    ✗ vmalloc() — needs vmalloc init (Layer 9)                  │
│    ✗ ioremap() — needs vmalloc                                 │
│                                                                │
│  What Comes Next:                                              │
│    → kmem_cache_init() (Layer 9): bootstrap SLUB allocator     │
│    → vmalloc_init() (Layer 9): virtual contiguous allocator    │
│    → Memory subsystem fully operational!                       │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 8

| Step | Function | What It Did |
|------|----------|-------------|
| 1 | `build_all_zonelists()` | Built zone fallback lists for allocation |
| 2 | `reset_all_zones_managed_pages()` | Zeroed all managed_pages counters |
| 3 | `memmap_init_reserved_pages()` | Marked reserved pages with PG_Reserved |
| 4 | `free_low_memory_core_early()` | Iterated all free memblock ranges |
| 5 | `__free_pages_core()` | Cleared PG_Reserved, set refcount=0 for free pages |
| 6 | `__free_one_page()` | Fed pages into buddy, merged with buddies |
| 7 | `totalram_pages_add()` | Set total available RAM counter |

**This is the most important transition in the entire boot memory sequence**. Before this
point, all memory allocation went through the simple memblock allocator. After this point,
the kernel uses the buddy allocator — the same allocator it will use for the entire
lifetime of the system.

The allocator transition:
```
Boot start ────── memblock_free_all() ────── System running
                        │
   memblock_alloc()     │     alloc_pages()
   memblock_reserve()   │     __get_free_pages()
                        │     kmalloc() (after SLUB)
                        │     vmalloc() (after vmalloc_init)
```

---

[← Layer 7: Zones, Nodes & Page Init](07_Zones_Nodes_and_Page_Init.md) | **Layer 8** | [Layer 9: Slab, vmalloc & Runtime →](09_Slab_Vmalloc_and_Runtime.md)
