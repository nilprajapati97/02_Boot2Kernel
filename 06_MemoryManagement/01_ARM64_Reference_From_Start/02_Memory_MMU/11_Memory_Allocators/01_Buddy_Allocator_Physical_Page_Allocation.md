# Buddy Allocator: Physical Page Allocation

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Buddy allocator: the Linux kernel's physical page allocator
  Manages ALL free physical memory (except reserved regions)
  Allocates 2^N contiguous pages (order N, N = 0..10)
  
  Why power-of-2 sizes?
    Efficient merging: two buddies of order N → one order N+1 block
    Simple address arithmetic: buddy of block at PA x = x XOR (PAGE_SIZE << N)
    Avoids external fragmentation for large blocks
  
  Orders:
    Order 0: 1 page = 4KB
    Order 1: 2 pages = 8KB
    Order 2: 4 pages = 16KB
    ...
    Order 10: 1024 pages = 4MB (MAX_ORDER-1)
  
  MAX_ORDER: default 11 (orders 0–10), giving max allocation 4MB
  ARM64: configurable, some platforms extend to higher orders for huge pages

Data structure:
  Per-zone, per-order free lists:
    struct zone {
        struct free_area  free_area[MAX_ORDER];
    };
    
    struct free_area {
        struct list_head    free_list[MIGRATE_TYPES];
        unsigned long       nr_free;   // number of free blocks of this order
    };
  
  MIGRATE_TYPES:
    MIGRATE_UNMOVABLE:  pages that can't be migrated (kernel data)
    MIGRATE_MOVABLE:    pages that can be migrated (user pages, file cache)
    MIGRATE_RECLAIMABLE: pages that can be reclaimed (page cache)
    MIGRATE_HIGHATOMIC: high-priority emergency allocations
    MIGRATE_CMA:        contiguous memory allocator pages
    MIGRATE_ISOLATE:    being migrated (isolated from allocator)
  
  Per-CPU page caches (pcppage):
    struct per_cpu_pageset: cache of order-0 pages per CPU
    Avoids spinlock on zone->lock for common order-0 allocations
    Linux: hot/cold page lists in pcp (per-cpu-pages)
```

---

## 2. Buddy Allocator Algorithm

```
Allocation of order-N block:

alloc_pages(gfp_mask, order):
  1. Select zone (ZONE_DMA / ZONE_NORMAL / ZONE_MOVABLE based on GFP flags)
  2. Check zone->free_area[order]:
     a. If free_list[migratetype] is non-empty:
          Remove first block from list
          Split remaining (if we took a higher-order block for order N):
            → No splitting needed (exact order match)
          Mark pages as allocated: page->private = 0, flags updated
          Return page
  3. If order-N list is empty:
     Find smallest available order M > N:
       for (m = order+1; m < MAX_ORDER; m++):
         if free_area[m].nr_free > 0: break
     Take one order-M block
     SPLIT: repeatedly split in half until we have order-N:
       for (high = m; high > order; high--):
         Take order-high block → split into two order-(high-1) blocks
         Put one half back on free_area[high-1] free list
         Keep the other half for next iteration
       Result: one order-N block + order-(order+1) block + ... + order-(M-1) block
               all put on respective free lists

Example (allocating order-2, only order-4 available):
  Before: free_area[4] has one block at PFN 0x1000
  Step 1: Remove PFN 0x1000 (order-4)
  Step 2: Split: PFN 0x1000 (order-3) + PFN 0x1008 (order-3) → put 0x1008 back
  Step 3: Split: PFN 0x1000 (order-2) + PFN 0x1004 (order-2) → put 0x1004 back
  Result: Return PFN 0x1000 (order-2, 4 pages)
  After:  free_area[3]: {0x1008}; free_area[2]: {0x1004}

Free (returning order-N block at PFN p):

free_pages(page, order):
  1. Find buddy PFN: buddy_pfn = pfn XOR (1 << order)
     (buddy = block that would merge with this block)
  2. Check if buddy is free and same order:
     buddy_page = pfn_to_page(buddy_pfn)
     if (PageBuddy(buddy_page) && buddy_order == order):
       → MERGE!
       Remove buddy from free_area[order]
       Combined block: min(pfn, buddy_pfn), order+1
       Recurse with new combined block at order+1
     else:
       → Add to free_area[order]
  3. Repeat until no merge possible (buddy not free, or MAX_ORDER reached)

Buddy identification:
  PageBuddy(page): checks PG_buddy flag in page->flags
  buddy_order(page): stored in page->private for buddy pages
```

---

## 3. GFP Flags Reference

```
GFP (Get Free Pages) flags control allocator behavior:

Allocation context:
  GFP_KERNEL:      Can sleep, can reclaim, standard kernel allocation
  GFP_ATOMIC:      Cannot sleep (interrupt/spinlock context), no reclaim
  GFP_NOWAIT:      Cannot sleep, no reclaim, no fail (best effort)
  GFP_NOIO:        Can sleep but no I/O (for I/O paths to avoid deadlock)
  GFP_NOFS:        Can sleep but no filesystem (for FS to avoid deadlock)
  GFP_USER:        User allocation (may be pageable)
  GFP_HIGHUSER:    User allocation, prefer high memory zones
  GFP_DMA:         Must be in DMA zone (low 16MB on x86; rare on ARM64)
  GFP_DMA32:       Must be in ZONE_DMA32 (< 4GB physical)

Behavioral modifiers:
  __GFP_ZERO:      Zero-fill allocated pages
  __GFP_NOWARN:    Don't print warning if allocation fails
  __GFP_RETRY_MAYFAIL: Retry but allow failure (softer than __GFP_NOFAIL)
  __GFP_NOFAIL:    Never fail (keep retrying indefinitely — dangerous!)
  __GFP_HIGH:      Use emergency reserves (for critical kernel paths)
  __GFP_MEMALLOC:  Use memory reserves even below watermarks
  __GFP_NOMEMALLOC: Don't use reserves
  __GFP_COLD:      Prefer cold (not in CPU cache) pages
  __GFP_MOVABLE:   Page is movable (for migration/compaction)
  __GFP_RECLAIMABLE: Page is reclaimable (slab, page cache)
  __GFP_ACCOUNT:   Charge against memory cgroup

ARM64 zone selection based on GFP:
  GFP_DMA:     ZONE_DMA  (< 1GB on many ARM64 boards)
  GFP_DMA32:   ZONE_DMA32 (< 4GB, useful for 32-bit DMA devices on 64-bit ARM)
  Default:     ZONE_NORMAL (most RAM)
  GFP_HIGHUSER_MOVABLE: ZONE_MOVABLE (separate zone for migration)
  
  ARM64 memory zones (typical 8GB system):
    ZONE_DMA:     0x00000000 – 0x40000000 (0–1GB)  [legacy DMA limitation]
    ZONE_NORMAL:  0x40000000 – 0x200000000 (1GB–8GB)
    ZONE_MOVABLE: (optional, for memory hotplug or migration)
```

---

## 4. Per-CPU Page Set (pcppage)

```
Per-CPU page cache (pcppage / pcp):
  Problem: zone->lock is a spinlock — contended on multi-CPU ARM64 servers
  Solution: each CPU caches order-0 pages in a lock-free list
  
  struct per_cpu_pages {
      int count;           // number of pages in this CPU's list
      int high;            // maximum (drain when exceeded)
      int batch;           // refill batch size
      struct list_head lists[MIGRATE_PCPTYPES];
  };
  
  Allocation (order 0 only):
    rmqueue_pcplist(zone, migratetype):
      Take from CPU's pcp list (NO spinlock needed)
      If list empty: refill from zone: rmqueue_bulk()
        Takes zone->lock, removes `batch` pages, releases lock
        Batch size: typically 31 pages (configurable)
  
  Free (order 0 only):
    free_unref_page_commit(page, pfn, migratetype):
      Add to CPU's pcp list (NO spinlock needed)
      If count > high: drain batch pages back to zone
  
  Effect: reduces zone->lock contention significantly
  ARM64 server (Neoverse N2, 64 cores): pcp is critical for performance
```

---

## 5. Interview Questions & Answers

**Q1: What is the "buddy" in the buddy allocator? How does the kernel find the buddy of a given block?**

The "buddy" is the block that, when merged with the current free block, forms a larger aligned block of the next order. Two blocks are buddies if and only if:
1. They are the same order (same size)
2. They are adjacent in memory
3. They are aligned to 2× their size

For a block at PFN `p` of order `n`, its buddy is at PFN `buddy = p XOR (1 << n)`.

Example: PFN 0x1004 (order 2) → buddy = 0x1004 XOR 4 = 0x1000. If 0x1000 is also free at order 2, they merge into order-3 block at PFN 0x1000.

The XOR trick works because an order-N block must be aligned to `2^N` pages. A block at address aligned to `2^(N+1)` has its lower half at `x` and upper half at `x + 2^N = x XOR 2^N`.

---

## 6. Quick Reference

| Order | Size | Use Case |
|---|---|---|
| 0 | 4 KB | kmalloc (small), single page alloc |
| 1 | 8 KB | Medium slab allocation |
| 2 | 16 KB | ARM64 16KB page TLB compatibility |
| 3 | 32 KB | Small DMA buffers |
| 4 | 64 KB | Medium DMA, kstack (16KB × 4) |
| 9 | 2 MB | Transparent huge pages (THP) |
| 10 | 4 MB | Max buddy alloc (MAX_ORDER-1) |

| /proc/buddyinfo format | Meaning |
|---|---|
| Node 0, zone Normal 100 50 25 ... | Order 0: 100 free, Order 1: 50 free, ... |
| High number at order 0, low at order 9 | Normal state (fragmented) |
| Low at all orders | Memory pressure! |
