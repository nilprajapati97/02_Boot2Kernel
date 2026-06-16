# Page Reclaim, LRU, and Swap

**Category**: Page Reclaim and Swap  
**Platform**: ARM64 (AArch64)

---

## 1. Memory Reclaim Overview

```
Memory reclaim: freeing pages to satisfy new allocation requests
  When needed: free memory falls below watermarks
  Who does it: kswapd (background) + direct reclaim (foreground)
  
  Memory zones and watermarks (per-zone):
    pages_high:   "comfortable" — kswapd can sleep
    pages_low:    wake kswapd to reclaim
    pages_min:    critical — direct reclaim (blocking)
    
    /proc/sys/vm/watermark_scale_factor: scale watermarks (default 10 = 0.1%)
    /proc/zoneinfo: current watermark values
  
  Reclaim path selection:
    alloc_pages():
      → available pages > pages_low: no reclaim needed
      → pages_min < available < pages_low: wake kswapd, try alloc
      → available < pages_min: enter direct reclaim path
        (current process is blocked until pages freed)

kswapd:
  Per-NUMA-node kernel thread: [kswapd0], [kswapd1], ...
  Woken by: wake_up_kswapd() from alloc_pages()
  Runs: balance_pgdat() → kswapd_shrink_node()
  Goal: bring zones above pages_high watermark
  
  balance_pgdat():
    For each zone (low → high priority):
      shrink_zone() → shrink_lruvec() → ... reclaim pages
    Repeat until all zones at or above pages_high
    Sleep: when all zones are comfortable
  
Direct reclaim:
  Called from: __alloc_pages_slowpath() (in allocation path)
  Blocks: current process blocked until pages freed
  Throttled: may use memory compaction before direct reclaim
  OOM: if direct reclaim fails repeatedly → OOM killer
```

---

## 2. LRU Lists

```
LRU (Least Recently Used) page lists: track page reclaim order

Four main LRU lists per lruvec:
  LRU_ACTIVE_ANON:    recently-accessed anonymous pages (heap, stack)
  LRU_INACTIVE_ANON:  older anonymous pages (candidates for swap)
  LRU_ACTIVE_FILE:    recently-accessed file-backed pages (page cache)
  LRU_INACTIVE_FILE:  older file-backed pages (candidates for drop)
  LRU_UNEVICTABLE:    mlocked pages, never reclaimed
  
  lruvec: per-node, per-cgroup structure holding all 5 LRU lists
    struct lruvec {
      struct list_head lists[NR_LRU_LISTS];
      unsigned long anon_cost;
      unsigned long file_cost;
      ...
    };
  
  Page lifecycle:
    First fault: page added to inactive list
    Accessed again (PTE access bit set): promoted to active list
    Not accessed for a while: rotated from active → inactive
    Still not accessed: reclaimed (swapped out or dropped)
  
  Access bit management:
    ARM64: hardware access flag in PTE (bit[10], AF=Access Flag)
    PTE Access Flag (AF): set by hardware on first access (ARMv8.1+)
    Linux check: ptep_test_and_clear_young() → checks+clears AF
    If AF was set: page recently accessed → keep in active list
    If AF was clear: page not recently accessed → demote to inactive

Page promotion/demotion:
  mark_page_accessed():
    Called by: do_wp_page, generic_file_read, etc.
    If inactive → promote to active list
    If active → mark as "recently accessed" (update aging)
  
  lru_cache_add():
    Initial page addition to LRU:
    Batch via per-CPU lru_pvec (14 pages) → flush to global LRU lists

LRU active/inactive ratio:
  Goal: keep ~⅔ active, ~⅓ inactive (enough cold pages to reclaim)
  shrink_active_list(): demote oldest active → inactive
  shrink_inactive_list(): reclaim oldest inactive pages

MGLRU (Multi-Generational LRU): Linux 6.1+
  Alternative to classic LRU
  Generations: multiple generations of pages (not just active/inactive)
  Better: handles large working sets with mixed access patterns
  ARM64: fully supported, CONFIG_LRU_GEN=y
  /sys/kernel/mm/lru_gen/: MGLRU tuning parameters
```

---

## 3. Page Reclaim Algorithm

```
shrink_lruvec(): core reclaim function for one lruvec

Entry points:
  kswapd → balance_pgdat → kswapd_shrink_node → shrink_node → shrink_lruvec
  direct reclaim → try_to_free_pages → do_try_to_free_pages → shrink_node → shrink_lruvec

shrink_lruvec(lruvec, sc):
  sc = scan_control {
    nr_to_reclaim: pages to reclaim
    gfp_mask:      allocation context
    priority:      reclaim urgency (0=highest, 12=lowest)
    may_swap:      allow swap
    may_writepage: allow writeback
  }
  
  Step 1: get_scan_count():
    Determine how many pages to scan from each list
    Ratio based on: anon vs file pages, swappiness setting
    High swappiness (60 default): willing to swap anon pages
    Low swappiness (0): prefer file pages, avoid swap
    
  Step 2: shrink_active_list():
    Scan active list
    For each page: check access bit (ptep_test_and_clear_young)
    Recently accessed: keep in active list
    Not accessed: move to inactive list
    
  Step 3: shrink_inactive_list():
    Scan inactive list
    For each page:
      File-backed page: can it be dropped from page cache?
        If clean: drop immediately (just remove from page cache)
        If dirty: writeback to disk → reclaim after writeback
      Anonymous page: swap it out?
        map_count > 0 (still mapped): add_to_swap() → swap_out
        map_count == 0 (already unmapped): free directly

Reclaim decisions:
  File clean page:       FREE immediately (just invalidate page cache entry)
  File dirty page:       WRITEBACK → wait → free (or skip, defer)
  Anonymous page:        SWAP OUT → compress or write to swap device → free
  Mapped anonymous page: unmap (ptep_clear_flush) → add to swap
  mlocked page:          SKIP (on LRU_UNEVICTABLE, never reclaimed)
  
  shrink_page_list(): actual per-page reclaim decision
    pageout(): initiate writeback for dirty pages
    try_to_unmap(): remove all PTEs pointing to this page
    add_to_swap(): write to swap, install swap_entry_t in PTE
    __delete_from_page_cache(): remove from page cache
    __free_pages(): return to buddy

Reclaim metrics:
  /proc/vmstat:
    pgreclaim_direct: pages reclaimed directly (blocking)
    pgreclaim_kswapd: pages reclaimed by kswapd
    pgmajfault: major faults (page was swapped out, brought back)
    pgpgout: pages written to swap/disk
    pgpgin: pages read from swap/disk
```

---

## 4. Swap Subsystem

```
Swap: page eviction to block storage when RAM is full

Swap space types:
  Swap partition: dedicated disk partition
    mkswap /dev/sda2; swapon /dev/sda2
  Swap file: regular file used as swap
    dd if=/dev/zero of=/swapfile bs=1M count=4096; mkswap /swapfile; swapon /swapfile
  zram: compressed RAM swap (in-memory, no disk)
    modprobe zram; echo $((4*1024*1024*1024)) > /sys/block/zram0/disksize
    mkswap /dev/zram0; swapon /dev/zram0

swap_entry_t in PTE:
  When page is swapped out:
    PTE content CHANGES from PA → swap entry
    Format: swap_entry_t = (type << SWP_TYPE_SHIFT) | offset
    type:   swap device index
    offset: page offset within swap device
  
  ARM64 swapped-out PTE:
    Present bit (bit[0]) = 0: page not present
    Remaining bits: encode swap_entry_t
    On access: page fault → PageSwapCache? → swap_in
  
  Swap path (eviction):
    add_to_swap():
      swap_map: track reference count for swap slot
      get_swap_page(): find free swap slot
      SetPageSwapCache(page): mark as in swap cache
    
    swap_writepage():
      submit_bio() to swap device
      Page: temporarily in swap cache (both PA in memory + swap entry)
    
    After writeback complete:
      page_remove_rmap(): remove all page mappings
      try_to_unmap_one(): install swap_entry_t in each PTE
      __delete_from_swap_cache(): remove from swap cache
      __free_page(): free physical page
  
  Swap-in (page fault on swapped-out page):
    do_swap_page():
      lookup_swap_cache(): check if still in swap cache (fast path)
      swapin_readahead(): read from swap device + prefetch neighbors
      Allocate new page, read from swap device
      set_pte_at(new_pte): install new PTE
      swap_free(): decrement swap_map reference count
      Free swap slot when swap_map[offset] = 0

zram swap (in-memory compressed):
  zram: each "swap write" → compress page (LZ4/ZSTD/LZO)
        store in zram's compressed pool
  Effective compression ratio: 2-4× (text/heap pages compress well)
  Benefit: no disk I/O, much faster than disk swap
  Use case: embedded (no disk), desktop (reduce disk wear), cloud VMs
  
  /sys/block/zram0/comp_algorithm: lz4, zstd, lzo-rle
  /sys/block/zram0/disksize: logical swap size
  /sys/block/zram0/mm_stat: memory usage stats
```

---

## 5. OOM Killer

```
OOM (Out of Memory) killer: last resort when memory cannot be reclaimed

OOM trigger path:
  __alloc_pages_slowpath():
    1. Try direct reclaim (multiple times)
    2. Try compaction
    3. If still failing: out_of_memory()
  
  out_of_memory():
    oom_kill_process():
      select_bad_process(): find victim process
      do_send_sig_info(SIGKILL): kill it
      __oom_kill_process(): force kill + log message

OOM score calculation (oom_badness()):
  Score = process_pages + swap_pages
  Modified by: /proc/<pid>/oom_score_adj (-1000 to +1000)
    +1000: always kill first (disposable processes)
    -1000: never kill (critical daemons like sshd)
    0: default
  
  Higher oom_score: more likely to be killed
  Heuristic: kill largest process to free most memory quickly
  
  /proc/<pid>/oom_score: current OOM score (read-only)
  /proc/<pid>/oom_score_adj: adjustment (read-write by process owner or root)
  
  OOM messages in dmesg:
    Out of memory: Killed process 1234 (firefox) total-vm:4GB, ...
    oom_kill_process: [firefox] score 850 or higher, score=920
  
  cgroup OOM:
    cgroup v2: memory.max = hard limit per cgroup
    On limit hit: cgroup OOM (kills within cgroup first)
    memory.oom.group = 1: kill entire cgroup together (not just one process)

Anti-OOM strategies:
  1. Set oom_score_adj = -500 for critical services
  2. Use cgroup memory limits (contain OOM to one service)
  3. earlyoom (userspace daemon): kill processes before kernel OOM
  4. Overcommit settings:
     /proc/sys/vm/overcommit_memory:
       0: heuristic (default) - allow reasonable overcommit
       1: always allow (no checking) - dangerous
       2: never overcommit beyond (RSS × overcommit_ratio + swap)
```

---

## 6. Interview Questions & Answers

**Q1: What are the LRU lists and when are pages promoted/demoted?**
Linux maintains 5 LRU lists per lruvec: Active Anonymous (recently accessed heap/stack), Inactive Anonymous (cold heap/stack, swap candidates), Active File (recently accessed page cache), Inactive File (cold page cache, drop candidates), Unevictable (mlocked). Promotion: `mark_page_accessed()` moves pages from inactive → active (called on file read, write fault). Demotion: `shrink_active_list()` checks ARM64 hardware access flag (AF bit in PTE); if not recently accessed, demotes active → inactive. Reclaim targets: inactive lists only.

**Q2: How does swapping work at the PTE level?**
When a page is swapped out: the physical PTE (with PA) is replaced by a `swap_entry_t` encoded in the PTE bits. ARM64: PTE bit[0] (Present) = 0 means page not present. Remaining bits encode swap type + swap offset. On next access: page fault fires (`do_swap_page()`). Kernel reads `swap_entry_t` from the PTE, locates the page in the swap device, reads it back, allocates a new physical page, installs a new PTE with the real PA, returns to user. The swap entry is freed from swap space (decrement `swap_map[offset]`).

**Q3: What is swappiness and how does it affect memory reclaim?**
Swappiness (`/proc/sys/vm/swappiness`, default 60) controls the ratio of anonymous vs file page reclaim. Higher swappiness: more willing to swap out anonymous (heap/stack) pages. Lower swappiness (near 0): avoid swap, prefer dropping file-backed pages instead. `swappiness=0`: never swap unless completely out of memory. `swappiness=100`: treat anon pages equally with file pages for reclaim. For databases (PostgreSQL): often set to 0 (because database handles its own caching and doesn't want kernel swapping its working set).

**Q4: What is the difference between kswapd and direct reclaim?**
`kswapd`: background daemon, runs asynchronously, woken when free memory drops below the `pages_low` watermark. Runs ahead of memory pressure — aims to keep zones at or above `pages_high`. Non-blocking for application threads. Direct reclaim: runs in the context of the allocating thread (`__alloc_pages_slowpath()`). Blocks the allocating thread until pages are freed. Triggered when free memory is below `pages_min`. More aggressive than kswapd (higher reclaim priority). Both call `shrink_lruvec()` internally — same reclaim code, different urgency levels.

**Q5: How does zram differ from disk swap?**
Disk swap: evicted pages written to disk (HDD: ~10ms, SSD: ~0.1ms). zram: pages compressed (LZ4/ZSTD) and stored in RAM (~1µs). 100-10,000× faster than disk swap. No I/O bandwidth consumed. Compressed ratio 2-4× → effectively 2-4× more "RAM" from the zram space. Trade-off: CPU overhead for compression/decompression (~0.1-1µs per page). Best use: embedded systems (no disk), desktop/laptop (reduce disk I/O), containers (fast page reclaim). For ARM64 embedded (e.g., Android): `zram` is default swap backend.

---

## 7. Quick Reference

| LRU List | Contents | Reclaim Action |
|---|---|---|
| Active Anon | Recently-accessed heap/stack | Demote → Inactive Anon |
| Inactive Anon | Cold heap/stack | Swap out |
| Active File | Recently-accessed page cache | Demote → Inactive File |
| Inactive File | Cold page cache | Drop (if clean) or writeback+drop |
| Unevictable | mlocked pages | Never reclaimed |

| Watermark | Value | Action |
|---|---|---|
| pages_high | > high | kswapd sleeps |
| pages_low | low-high | wake kswapd |
| pages_min | < min | direct reclaim |
