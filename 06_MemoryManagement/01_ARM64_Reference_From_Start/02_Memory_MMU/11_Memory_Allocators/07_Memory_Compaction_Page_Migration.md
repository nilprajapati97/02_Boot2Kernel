# Memory Compaction and Page Migration

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Physical memory fragmentation problem:
  Over time, the buddy allocator produces "external fragmentation":
    Many free pages scattered throughout physical memory
    BUT: no large contiguous free region available
  
  Example: 4GB RAM, 50% free, but no 2MB contiguous block for huge pages:
    [USED][FREE][USED][FREE][USED][FREE]...
    2048 free pages (8MB total) but none are contiguous order-9 blocks

Memory compaction: defragment physical memory
  Strategy: MOVE movable pages to LOW addresses (pack them together)
            This FREES UP high addresses (large contiguous free regions)
  
  ARM64 importance: huge pages (2MB blocks) critical for TLB efficiency
    Neoverse N2 server: L1 TLB covers 32 4KB entries vs 32 2MB entries
    2MB page: 512× more memory per TLB entry
    Fragmentation → no 2MB pages → TLB pressure → performance degradation

Compaction types:
  1. Synchronous: triggered by a failing high-order allocation
     alloc_pages(order=9) fails → try_to_compact_pages() before OOM
  
  2. Asynchronous: kcompactd daemon (per NUMA node)
     Runs when watermarks trigger need for compaction
     CONFIG_COMPACTION must be enabled
  
  3. Manual: echo 1 > /proc/sys/vm/compact_memory
     Triggers full compaction of all zones on all NUMA nodes
```

---

## 2. Compaction Algorithm

```
compact_zone(zone, cc):
  cc = compact_control {
    .order = target_order,        // we want this order block
    .gfp_mask = gfp_mask,
    .zone = zone,
    .sync = MIGRATE_SYNC or MIGRATE_ASYNC,
  };
  
  DUAL-CURSOR APPROACH:
    cc->migrate_pfn = zone->zone_start_pfn  (scan from bottom: find MOVABLE pages)
    cc->free_pfn    = zone_end_pfn(zone)    (scan from top: find FREE pages)
    
    Goal: migrate pages from migrate_pfn side to free_pfn side
    Stop: when migrate_pfn >= free_pfn (cursors meet in middle)

  MIGRATION SCANNER (isolate_migratepages):
    Starts at cc->migrate_pfn, scans upward
    For each page:
      If MIGRATE_MOVABLE or MIGRATE_CMA: candidate for migration
      isolate_migratepage(page):
        Check: PageLRU(page) [must be on LRU to move]
        Check: NOT PG_mlocked (can't move locked pages)
        Check: page_mapcount > 0 (has PTEs to update)
        Move to cc->migratepages list
        Set PG_isolated
    Advance cc->migrate_pfn

  FREE PAGE SCANNER (isolate_freepages):
    Starts at cc->free_pfn, scans downward
    For each page order N (high to low):
      If page is on buddy free list of ORDER >= target_order:
        Remove from buddy (PageBuddy)
        Add to cc->freepages list
    Advance cc->free_pfn

  MIGRATION:
    migrate_pages(&cc->migratepages, compaction_alloc, ...):
      For each isolated migratepage:
        new_page = compaction_alloc(page, cc):
          Take a free page from cc->freepages list
          The FREE page is in the HIGH address (top of zone) area
        
        unmap_and_move(page, new_page):
          try_to_unmap(page):
            rmap_walk → for each PTE: pte_unmap, pte_clear
          move_to_new_page(page, new_page):
            Migrate page content:
              For anonymous page: copy_highpage(new_page, page)
              For file page:      mapping->a_ops->migratepage() or fallback copy
              page->mapping moved: new_page->mapping = page->mapping
          install_new_page(new_page, old page's mapping):
            replace_page_cache_page (for file pages)
            OR: update anon_vma rmap
          re-map: for each VMA that mapped old page:
            set_pte_at(mm, addr, pte, mk_pte(new_page, vma->vm_page_prot))
          TLB flush:
            flush_tlb_page(vma, addr): TLBI VAE1IS on ARM64
          
          Free old page back to buddy: put_page(old_page) → buddy

  RESULT: 
    Top of zone: large contiguous free regions (for order-9 huge page alloc)
    Bottom of zone: packed movable pages
```

---

## 3. ARM64 TLB Cost of Compaction

```
ARM64 TLB flush overhead during compaction:

Per-page flush (flush_tlb_page):
  TLBI VAE1IS, Xt    // By VA, inner-shareable (all CPUs in cluster)
  DSB ISH             // Wait for all CPUs to complete invalidation

For 64 pages migrated (256KB range):
  64 × TLBI VAE1IS
  + 64 × DSB ISH
  
  Each TLBI+DSB on 64-core ARM64 server: ~1–5 µs
  Total: 64–320 µs for TLB flush alone
  Plus: page copy cost = 64 × memcpy(4KB) = 256KB copy

Optimization: batch TLB flushes
  Linux: collects all VA ranges to flush, does ONE batch TLBI:
  tlb_gather_mmu(&tlb, mm):
    Collect all {mm, start, end} ranges
  tlb_flush_mmu(&tlb):
    flush_tlb_range(vma, start, end): one TLBI range covering all pages
    ARM64: TLBI VAE1IS for each page in range (or range-based if FEAT_TLBIRANGE)
    FEAT_TLBIRANGE (ARMv8.4): TLBI VALE1IS instruction with range
      Eliminates per-page TLBI loop for large ranges

Compaction cost summary:
  CPU overhead: ~2–10% CPU for kcompactd (1 thread per NUMA node)
  Memory bandwidth: copying + remap overhead
  Latency: synchronous compaction: can add 10ms–1s to allocation latency
  
  ARM64 NUMA server: kcompactd runs on each NUMA node independently
    Compaction is NUMA-aware: prefers to move pages within same NUMA node
    Cross-NUMA migration adds NUMA latency on top of migration cost
```

---

## 4. Fragmentation Index

```
/proc/extfrag_index:
  Fragmentation index per zone per order
  0:   allocation failure is due to LOW memory (need to reclaim)
  100: allocation failure is due to FRAGMENTATION (have memory, but not contiguous)
  
  Used to decide: compact or reclaim?
  If extfrag_index[order] > 500: compaction will likely help
  If extfrag_index[order] < 500: reclaim first

/proc/buddyinfo:
  Node 0, zone   Normal  256  128  64  32  16   8   4   2   1   0   0
  Columns = order 0..10 (free blocks of each order)
  Last column (order 10) = 0: no 4MB contiguous free blocks!

/sys/kernel/debug/extfrag/extfrag_index:
  More detailed per-zone-per-order fragmentation index

vm.extfrag_threshold: sysctl
  Default: 500
  If fragmentation index > threshold: trigger compaction instead of reclaim
```

---

## 5. Interview Questions & Answers

**Q1: Can compaction always succeed? What prevents pages from being migrated?**

No, compaction can fail. Several conditions prevent page migration:

1. **Pages with elevated `page_mapcount`**: if a page is mapped by many processes (shared library), migration is complex but usually possible.

2. **`PG_mlocked` (mlock'd) pages**: user process called `mlock()`. These pages are pinned in physical memory — the kernel GUARANTEES they won't be swapped or migrated. CMA and compaction cannot migrate them.

3. **Kernel slab pages (UNMOVABLE)**: `struct kmem_cache` objects are in MIGRATE_UNMOVABLE pages. Direct kernel pointers (not PTE-based) reference them — migrating would invalidate all those pointers.

4. **Pages with FOLL_PIN**: pages pinned via `pin_user_pages()` (used by RDMA, vfio, io_uring). These have elevated refcount that prevents migration.

5. **Pages with special mappings**: some drivers register `pgmap` callbacks for device memory — these can't be migrated.

6. **Concurrent writers**: if a page is being faulted or written during migration, migration may fail and retry or give up.

The result: in a system with many mlock'd or pinned RDMA buffers, compaction may be unable to create the large contiguous regions needed for huge pages.

**Q2: What is the difference between memory compaction and memory reclaim?**

Both try to make memory available for new allocations, but they differ fundamentally:

**Memory Reclaim** (`kswapd`, direct reclaim):
- FREES memory by writing dirty pages to swap/disk, discarding clean page cache
- Reduces total used memory
- Pages that were in use are REMOVED (not just relocated)
- Appropriate when: system is actually low on memory

**Memory Compaction** (kcompactd, `compact_memory` sysctl):
- REORGANIZES memory without freeing any
- Total used memory stays the same; only physical placement changes
- MOVABLE pages are packed toward one end, FREEING large contiguous blocks
- Appropriate when: fragmented but not actually low on memory

Kernel heuristic (based on `extfrag_index`):
- If fragmentation index < threshold (500): problem is too little free memory → reclaim
- If fragmentation index >= threshold: problem is fragmentation → compact

On a well-tuned ARM64 server with THP enabled, `kcompactd` prevents fragmentation proactively, reducing the need for expensive synchronous compaction during huge page allocation.

---

## 6. Quick Reference

| Compaction Trigger | Mechanism |
|---|---|
| High-order alloc fails | Synchronous: try_to_compact_pages() |
| Watermark crossed (low) | Asynchronous: kcompactd wakeup |
| Manual trigger | `echo 1 > /proc/sys/vm/compact_memory` |
| CMA alloc | alloc_contig_range() does internal compaction |

| Migration Type | Description |
|---|---|
| MIGRATE_SYNC | Wait for page writeback to complete |
| MIGRATE_SYNC_LIGHT | Like SYNC but skip pages under writeback |
| MIGRATE_ASYNC | Skip busy pages (for kcompactd background) |

| Fragmentation Indicators | Location |
|---|---|
| extfrag_index (0-100 per order) | /proc/extfrag_index |
| Free blocks per order | /proc/buddyinfo |
| CMA stats | /proc/vmstat: cma_alloc_* |
| Compaction stats | /proc/vmstat: compact_* |
