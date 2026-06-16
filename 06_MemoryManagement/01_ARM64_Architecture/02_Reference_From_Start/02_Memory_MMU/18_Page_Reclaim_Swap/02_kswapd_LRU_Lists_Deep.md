# kswapd and LRU Lists Deep Dive

**Category**: Page Reclaim and Swap  
**Platform**: ARM64 (AArch64)

---

## 1. Watermark and kswapd Overview

```
Memory watermarks (per zone):
  pages_min:  emergency threshold — direct reclaim + OOM
  pages_low:  kswapd wakeup threshold
  pages_high: kswapd target — stop reclaiming

/proc/zoneinfo shows per-zone watermarks:
  Node 0, zone   Normal
    pages free     2048000
    min         102400
    low         128000
    high        153600

kswapd lifecycle:
  1. Allocator (alloc_pages) checks: free < pages_low?
  2. If yes: wakeup_kswapd(zone, gfp_mask, order, classzone_idx)
             → wakes kswapd thread for this NUMA node
  3. kswapd: balance_pgdat() loop
     → scans zones top-down (high to DMA)
     → reclaims pages until free >= pages_high
     → then sleeps again

kswapd per-node:
  One kswapd kthread per NUMA node: [kswapd0], [kswapd1], ...
  pgdat->kswapd = the thread
  pgdat->kswapd_classzone_idx = highest zone that triggered wakeup
  pgdat->kswapd_order = order of allocation that triggered wakeup
```

---

## 2. kswapd balance_pgdat() Loop

```c
// mm/vmscan.c

static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx)
{
    int i;
    unsigned long nr_soft_reclaimed;
    unsigned long nr_soft_dirty;
    unsigned long pflags;
    unsigned long nr_boost_reclaim;
    unsigned long zone_boosts[MAX_NR_ZONES] = { };
    bool boosted;
    struct zone *zone;
    struct scan_control sc = {
        .gfp_mask = GFP_KERNEL,
        .order = order,
        .may_writepage = !laptop_mode,  // allow writeback?
        .may_unmap = 1,     // allow unmapping pages?
        .may_swap = 1,      // allow swapping?
        .reclaim_idx = highest_zoneidx,  // highest zone to reclaim
    };
    
    do {
        // Priority: 0 (most aggressive) to DEF_PRIORITY=12 (gentlest)
        // Lower priority = scan more pages per LRU list
        sc.priority = DEF_PRIORITY;   // start gentle
        
        do {
            // For each zone from highest_zoneidx down to 0:
            for (i = highest_zoneidx; i >= 0; i--) {
                zone = pgdat->node_zones + i;
                
                // Is this zone already balanced?
                if (zone_balanced(zone, sc.order, i))
                    continue;
                
                // Try to reclaim from this zone's LRU lists:
                kswapd_shrink_node(pgdat, &sc);
            }
            
            // Did we balance all zones?
            if (pgdat_balanced(pgdat, sc.order, highest_zoneidx))
                goto out;
            
            // Increase aggressiveness:
            sc.priority--;
            
        } while (sc.priority >= 1);  // stop before priority 0
        
        // If still not balanced after priority 1:
        // emergency: allow more aggressive reclaim
        
    } while (true);  // loop until balanced
    
out:
    return sc.order;
}

// Zone balance check:
zone_balanced(zone, order, classzone_idx):
    // Returns true if zone has enough free pages at given order
    // Checks: zone->free_pages >= zone->watermark[WMARK_HIGH]
    //         AND: there are free_pages at 'order' or higher
```

---

## 3. LRU Lists Architecture

```
Five LRU lists (struct lruvec contains all):

LRU_INACTIVE_ANON:  Anonymous pages, recently evicted from active list
LRU_ACTIVE_ANON:    Anonymous pages, recently accessed
LRU_INACTIVE_FILE:  File-backed pages, recently evicted from active
LRU_ACTIVE_FILE:    File-backed pages, recently accessed
LRU_UNEVICTABLE:    Mlocked pages (never reclaimed)

struct lruvec {
    struct list_head lists[NR_LRU_LISTS];  // 5 lists
    spinlock_t lru_lock;                   // protects all lists
    // Scan statistics:
    unsigned long anon_cost;  // cost of scanning anon pages
    unsigned long file_cost;  // cost of scanning file pages
    // (used to balance scan ratio between anon vs file)
    
    atomic_long_t nonresident_age;  // for recency tracking
    unsigned long refaults[WORKINGSET_NR_TYPES]; // refault stats
    
    struct pglist_data *pgdat;  // back pointer to node
};

// In Linux 5.x: one lruvec per memcg × per zone
// In Linux 6.x: one lruvec per memcg × per node (MGLRU)

Page to LRU placement:
  New anonymous page (fault):
    → LRU_ACTIVE_ANON (starts active; will be moved to inactive on scan)
  
  New file page (read):
    → LRU_INACTIVE_FILE (file pages start inactive)
    → If accessed again: → LRU_ACTIVE_FILE
  
  Accessed inactive page:
    mark_page_accessed(page):
      If page is on LRU_INACTIVE_*:
        folio_set_referenced(folio)  // set a reference mark
      If page is on LRU_INACTIVE_* AND already has reference:
        → move to LRU_ACTIVE_*  (two-touch promotion)
  
  ARM64 AF (Access Flag) and LRU:
    PTE bit[10] = AF (Access Flag)
    On first access to page: MMU sets AF=1 in PTE (hardware update)
    When scanning LRU (shrink_active_list):
      pte_young(pte): check if AF=1 (page was accessed)
      If AF=1: page is "young" → keep on active list (clear AF for next check)
      If AF=0: page was NOT accessed since last scan → move to inactive
    
    ptep_clear_young(mm, addr, pte):  // clear AF=1 → AF=0
      ARM64: TLBI + clear bit[10] in PTE
      OR: use hardware dirty log (FEAT_HAFDBS) for automatic AF tracking
```

---

## 4. shrink_lruvec() — LRU Reclaim Core

```c
// mm/vmscan.c

static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
    unsigned long nr[NR_LRU_LISTS];
    unsigned long targets[NR_LRU_LISTS];
    
    // Step 1: Calculate how many pages to scan from each list
    get_scan_count(lruvec, sc, nr);
    // nr[LRU_INACTIVE_ANON] = N_anon_inactive to scan
    // nr[LRU_ACTIVE_ANON]   = N_anon_active to scan
    // nr[LRU_INACTIVE_FILE] = N_file_inactive to scan
    // nr[LRU_ACTIVE_FILE]   = N_file_active to scan
    
    // Step 2: Shrink active lists (move cold pages to inactive)
    shrink_active_list(nr[LRU_ACTIVE_ANON], lruvec, sc, LRU_ACTIVE_ANON);
    shrink_active_list(nr[LRU_ACTIVE_FILE], lruvec, sc, LRU_ACTIVE_FILE);
    
    // Step 3: Shrink inactive lists (actually reclaim pages)
    shrink_inactive_list(nr[LRU_INACTIVE_ANON], lruvec, sc, LRU_INACTIVE_ANON);
    shrink_inactive_list(nr[LRU_INACTIVE_FILE], lruvec, sc, LRU_INACTIVE_FILE);
    
    // Iterate until enough pages reclaimed or lists exhausted
}

get_scan_count():
  // Determines anon vs file scan ratio based on swappiness
  // swappiness = 0:   never scan anon (no swap)
  // swappiness = 60:  scan anon 60%, file 40% when both sets equal
  // swappiness = 200: very aggressive anon scanning
  
  arm64_total_anon = lruvec_lru_size(lruvec, LRU_ACTIVE_ANON)
                   + lruvec_lru_size(lruvec, LRU_INACTIVE_ANON)
  total_file = ... (similar for file pages)
  
  Fraction = swappiness / (200 - swappiness)   // anon:file ratio
  nr_anon_to_scan = total_anon × Fraction × (1/priority)
  nr_file_to_scan = total_file × (1-Fraction) × (1/priority)
```

### 4.1 shrink_inactive_list() — Actual Page Reclaim

```c
shrink_inactive_list(nr_to_scan, lruvec, sc, lru):
    // Take pages from tail of inactive list:
    isolate_lru_pages(nr_to_scan, lruvec, &page_list, ...);
    // page_list: batch of pages removed from LRU (temporarily isolated)
    
    // For each isolated page, call shrink_page_list():
    nr_reclaimed = shrink_page_list(&page_list, pgdat, sc, &stat, false);
    
    // Move unreclaimable pages back to appropriate LRU:
    putback_inactive_pages(lruvec, &page_list);
    
    // Update stats: sc->nr.reclaimed += nr_reclaimed

shrink_page_list():
    // Decision per page:
    for each page in page_list:
        
        if page_referenced(page):  // check AF bit, pte_young()
            // Page was recently accessed → not cold enough
            → move to active list (folio_activate())
            continue
        
        if page is dirty and is file-backed:
            if (sc->may_writepage):
                → add to writeback list (async IO)
                → keep in inactive list (retry after writeback)
            else:
                → skip (cannot reclaim dirty page without writeback)
            continue
        
        if page is anonymous:
            if (sc->may_swap and swap space available):
                → add_to_swap(page): create swap entry in PTE
                   ARM64: PTE becomes swap_entry (present=0, swap_offset in bits)
                → try_to_unmap(page): remove all PTEs pointing to this page
                → actually free from memory: __free_pages()
            else:
                → cannot reclaim (no swap, skip)
            continue
        
        if page is file-backed and clean:
            → try_to_unmap(page): remove all PTEs
            → truncate_cleanup_page(): remove from page cache
            → __free_pages(): free the physical page
```

---

## 5. MGLRU (Multi-Generation LRU) — Linux 6.1+

```
Problem with classic 4-LRU approach:
  Inactive/Active distinction is too coarse
  Global LRU: cannot distinguish "accessed 1s ago" vs "accessed 1h ago"
  Result: thrashing (evicting hot pages) under memory pressure

MGLRU design:
  Multiple "generations" per LRU type
  Newest generation: most recently accessed pages
  Oldest generation: candidates for eviction
  
  struct lru_gen_folio {
    // Folio pointers organized by generation:
    struct list_head lists[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
    unsigned long nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
    
    unsigned long min_seq[ANON_AND_FILE];  // oldest generation
    unsigned long max_seq;                  // newest generation
    // max_seq - min_seq = number of active generations (typically 4)
  };

MGLRU aging:
  lru_gen_age_node(): called periodically
  → walks MMU page tables looking for AF=1 bits
  → "ages" pages: clears AF and notes the generation age
  → pages with recently set AF: moved to new generation
  → pages not accessed: stay in old generation
  
  ARM64 advantage: FEAT_HAFDBS (Hardware Access Flag / Dirty Bit Support)
    Hardware can set AF bits without software intervention
    MGLRU uses: MMU_GATHER + hardware dirty tracking for efficient aging
    HW_A bit in VTCR_EL2 / TCR_EL1: enables hardware AF updates

MGLRU eviction:
  Evict from oldest generation (min_seq):
  → Pages in oldest generation are coldest → evict first
  → If oldest gen too small: run aging to create more cold candidates
  
  Result: better accuracy than 4-LRU
          measured: 10-30% improvement in workloads with temporal locality

Control:
  /sys/kernel/mm/lru_gen/enabled: 0x7 = enable MGLRU
  /sys/kernel/mm/lru_gen/min_ttl_ms: minimum age before eviction
```

---

## 6. Interview Q&A

**Q1: At what watermark does kswapd wake up, and what is its target?**
kswapd wakes when zone free pages drop below `pages_low` (the "low watermark"). It then runs `balance_pgdat()` in a loop, reclaiming pages until free pages reach `pages_high` (the "high watermark"). `pages_min` is the emergency threshold: if free pages drop below this while kswapd is running, direct reclaim (in the allocating task's context) kicks in simultaneously. Setting: `min_free_kbytes` sysctl controls the base minimum. Low/high are derived from min: typically `low = min × 1.25`, `high = min × 1.5` (scaled by `watermark_scale_factor`).

**Q2: What is the difference between LRU_ACTIVE and LRU_INACTIVE lists?**
ARM64 uses a "two-touch" promotion policy: a new page goes to INACTIVE. If it is accessed again (AF bit set again), it is promoted to ACTIVE. Active pages are protected from immediate reclaim — the scanner first moves cold ACTIVE pages back to INACTIVE (via `shrink_active_list()`), then reclaims from INACTIVE (via `shrink_inactive_list()`). This two-list structure ensures frequently-accessed pages (active) are protected while recently unused pages (inactive) are the reclaim targets. The ratio of active to inactive is enforced: too many active pages → more aggressive demotion.

**Q3: How does ARM64's Access Flag (AF bit) integrate with LRU reclaim?**
`PTE bit[10]` is the AF (Access Flag). On first access to a page, ARM64 MMU hardware sets AF=1 automatically (write to the PTE in memory). The LRU scanner (`shrink_active_list()`) reads AF via `pte_young(pte)` to determine if a page was accessed recently: if AF=1 → page is "young" → keep it active. The scanner then clears AF (`ptep_clear_young()` = clear bit[10] + TLB flush for the entry). Next scan: if AF stayed 0 → page was not accessed → demote to inactive list. This hardware-assisted age tracking is more accurate than software-only approaches on other architectures.

**Q4: What is MGLRU and why was it added to Linux 6.1?**
MGLRU (Multi-Generation LRU) replaces the classic 4-list LRU with a generational approach. Classic LRU: only two states per type (active/inactive) — cannot distinguish "accessed 1 second ago" from "accessed 1 hour ago." MGLRU: maintains N generations (typically 4), where generation number reflects recency. Eviction: always targets the oldest generation. Aging: periodically scans page tables looking for AF=1 → promotes those pages to the newest generation. Results: 10-30% reduction in page cache thrashing under realistic workloads. ARM64-specific benefit: FEAT_HAFDBS allows hardware to set AF bits without trapping to EL1 → efficient large-scale aging without per-page software overhead.

**Q5: What happens to a dirty anonymous page during memory reclaim?**
A dirty anonymous page is a page that was written to (modified) but not yet swapped. Reclaim path: (1) `add_to_swap(page)`: allocate a swap slot (`swap_entry_t`), write swap location into the PTE (present bit=0, swap offset in remaining bits). (2) ARM64 PTE becomes: bits[63:2] = swap entry encoding, bit[0]=0 (not present). (3) `try_to_unmap(page)`: walk reverse mappings (rmap) → clear all PTEs pointing to this page → TLB flush. (4) Schedule writeback: page is written to swap device asynchronously. (5) Once writeback completes: `__free_pages(page, 0)` — physical page returned to buddy. On next access: page fault → `do_swap_page()` → swap page in from swap device.

---

## 7. Quick Reference

| LRU List | Populated From | Evicted To |
|---|---|---|
| ACTIVE_ANON | New anon faults | INACTIVE_ANON (cold demotion) |
| INACTIVE_ANON | Demotion from active | Swap → Freed |
| ACTIVE_FILE | Accessed twice | INACTIVE_FILE (cold demotion) |
| INACTIVE_FILE | New file reads | Page cache drop → Freed |
| UNEVICTABLE | mlock() | Never reclaimed |

| sysctl | Default | Effect |
|---|---|---|
| `vm.swappiness` | 60 | Anon vs file reclaim ratio |
| `vm.min_free_kbytes` | Auto | Base watermark for all zones |
| `vm.watermark_scale_factor` | 10 | Multiplier for low/high from min |
| `vm.vfs_cache_pressure` | 100 | Bias toward reclaiming inode/dentry cache |
