# `setup_per_cpu_pageset()` — Per-CPU Page Caches

## Purpose

Allocates per-CPU page sets (also called "per-CPU hot-page caches" or "pcp lists") for each memory zone on each CPU. These caches allow page allocation and freeing without acquiring the global zone lock, dramatically improving allocation performance on multi-core systems.

## Source File

`mm/page_alloc.c`

```c
void __init setup_per_cpu_pageset(void)
{
    struct zone *zone;

    for_each_populated_zone(zone)
        setup_zone_pageset(zone);

    vm_total_pages = nr_free_pagecache_pages();
}
```

## The Problem: Zone Lock Contention

Without per-CPU caches, every `alloc_pages()` and `free_pages()` call must:
1. Lock the zone spinlock
2. Modify the free_area lists
3. Unlock

On a 128-core system with a high-throughput workload, this becomes a severe bottleneck.

## The Solution: Per-CPU Page Lists (PCP)

Each CPU has a small cache of pages per zone:

```c
struct per_cpu_pageset {
    struct per_cpu_pages pcp;
    /* stats */
};

struct per_cpu_pages {
    int count;         // Current number of pages in the list
    int high;          // Drain when count > high
    int batch;         // Pages to move to/from zone at once
    struct list_head lists[MIGRATE_PCPTYPES]; // Pages by migratetype
};
```

### Allocation Fast Path

```
alloc_pages(GFP_KERNEL, 0):  // Order-0 = single page
  → check per_cpu_pages.count > 0
  → YES: pop page from list, count--, return (no lock!)
  → NO:  take zone lock, move `batch` pages to PCP, unlock, retry
```

### Free Fast Path

```
free_page(page):
  → add page to per_cpu_pages list, count++
  → if count > high: take zone lock, drain `batch` pages back, unlock
```

## Typical PCP Parameters

For a zone with 8GB:
- `batch` ≈ 31 (pages moved to/from zone per refill/drain)
- `high` ≈ 186 (maximum pages in PCP before drain)

These values are computed based on zone size to balance memory overhead vs lock contention.

## Pre-conditions

- Buddy allocator operational (`mm_core_init()` complete)
- Per-CPU areas allocated
- `kmalloc()` available (for `per_cpu_pageset` allocation)

## Post-conditions

- All zone+CPU combinations have per-CPU page caches
- Page allocation is lock-free in the common case (order-0, per-CPU cache hit)
- `vm_total_pages` reflects actual available pages

## Cross-references

- [Phase overview](../README.md)
- [mm_core_init](mm_core_init/README.md) — sets up zones
