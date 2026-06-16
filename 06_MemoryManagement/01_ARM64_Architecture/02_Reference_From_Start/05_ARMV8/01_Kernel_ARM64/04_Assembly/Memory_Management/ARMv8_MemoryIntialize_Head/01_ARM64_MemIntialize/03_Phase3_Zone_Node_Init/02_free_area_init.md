# `free_area_init()` — Zone PFN Calculation & Page Initialization

**Source:** `mm/mm_init.c` lines 1700–1780
**Phase:** Zone & Node Init
**Memory Allocator:** Memblock (heavy user — allocates struct page arrays)
**Called by:** `mm_core_init_early()`
**Calls:** `arch_zone_limits_init()`, `sparse_init()`, `free_area_init_node()`

---

## What This Function Does

This is the **critical function** that transforms memblock's raw region data into the structured zone/node/page hierarchy needed by the buddy allocator:

1. Calculates exact PFN boundaries for each zone
2. Initializes SPARSEMEM (allocates `struct page` arrays)
3. For each NUMA node, initializes `pglist_data` and zone structures
4. Initializes all `struct page` entries for the system

---

## How It Works With Memory

### Memory It Allocates (from memblock)

| What | Size Formula | Example (4GB, 4KB pages) |
|------|-------------|--------------------------|
| `struct page[]` | 64 bytes × total_pages | 64 × 1,048,576 = 64 MB |
| `mem_section[]` | ~1 KB per section | ~8 KB for 4GB |
| vmemmap page tables | PGD/PUD/PMD/PTE pages | ~64 KB |
| Pageblock bitmap | 4 bits per pageblock (2MB) | ~1 KB |
| Zone padding | Cache-line aligned | < 1 KB |

**The `struct page` array is the largest single allocation from memblock** — typically 1.5% of total RAM.

---

## Step-by-Step Execution

### Step 1: Get Zone Boundaries from Architecture

```c
void __init free_area_init(void)
{
    unsigned long max_zone_pfn[MAX_NR_ZONES];
    memset(max_zone_pfn, 0, sizeof(max_zone_pfn));

    arch_zone_limits_init(max_zone_pfn);
```

`arch_zone_limits_init()` fills in the maximum PFN for each zone:

```c
// ARM64 implementation
void __init arch_zone_limits_init(unsigned long *max_zone_pfn)
{
    max_zone_pfn[ZONE_DMA]    = PFN_DOWN(zone_dma_limit);
    max_zone_pfn[ZONE_DMA32]  = PFN_DOWN(zone_dma32_limit);
    max_zone_pfn[ZONE_NORMAL] = max_pfn;
}
```

**Example:**

```
max_zone_pfn[ZONE_DMA]    = 0x40000   (1 GB limit)
max_zone_pfn[ZONE_DMA32]  = 0x100000  (4 GB limit)
max_zone_pfn[ZONE_NORMAL] = 0x180000  (6 GB — end of RAM)
```

---

### Step 2: Initialize SPARSEMEM

```c
    sparse_init();
```

**See:** [03_sparse_init.md](03_sparse_init.md) for full details.

**Summary:** Allocates `struct page` arrays for all memory sections and maps them into the vmemmap virtual address region. After this, `pfn_to_page(pfn)` works for any valid PFN.

---

### Step 3: Calculate Zone PFN Ranges

```c
    // Calculate lowest and highest PFN for each zone
    for (i = 0; i < MAX_NR_ZONES; i++) {
        if (i == ZONE_MOVABLE)
            continue;

        arch_zone_lowest_possible_pfn[i] =
            (i == 0) ? find_min_pfn_with_active_regions()
                     : arch_zone_highest_possible_pfn[i-1];

        arch_zone_highest_possible_pfn[i] = max_zone_pfn[i];
    }
```

**Result:**

```
Zone        | Lowest PFN | Highest PFN | Size
────────────┼────────────┼─────────────┼──────
ZONE_DMA    | 0x40000    | 0x40000     | varies
ZONE_DMA32  | 0x40000    | 0x100000    | up to 3 GB
ZONE_NORMAL | 0x100000   | 0x180000    | 2 GB
ZONE_MOVABLE| (calculated separately) | varies
```

---

### Step 4: Determine ZONE_MOVABLE Boundaries

```c
    find_zone_movable_pfns_for_nodes();
```

`ZONE_MOVABLE` is a special zone containing only pages that can be migrated (moved). Used for:
- **Memory hotplug** — pages in MOVABLE zone can be migrated away before removing DIMMs
- **Huge page allocation** — compaction can always succeed in MOVABLE zone
- Configured via `kernelcore=` or `movablecore=` boot parameters

---

### Step 5: Print Zone Ranges

```c
    // Kernel log output
    for (i = 0; i < MAX_NR_ZONES; i++) {
        pr_info("  %-8s %8lu -> %8lu\n",
                zone_names[i],
                arch_zone_lowest_possible_pfn[i],
                arch_zone_highest_possible_pfn[i]);
    }
```

**dmesg output:**

```
[    0.000000] Zone ranges:
[    0.000000]   DMA      [mem 0x40000000-0xffffffff]
[    0.000000]   DMA32    [mem 0x100000000-0x17fffffff]
[    0.000000]   Normal   empty
[    0.000000]   Movable  empty
```

---

### Step 6: Initialize Each NUMA Node

```c
    for_each_node(nid) {
        pg_data_t *pgdat = NODE_DATA(nid);
        free_area_init_node(nid);

        // Check if node has any memory
        if (pgdat->node_present_pages)
            node_set_state(nid, N_MEMORY);
    }
```

---

## `free_area_init_node()` — Per-Node Initialization

```c
static void __init free_area_init_node(int nid)
{
    pg_data_t *pgdat = NODE_DATA(nid);
    unsigned long start_pfn, end_pfn;

    // Get this node's PFN range
    get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);

    pgdat->node_id = nid;
    pgdat->node_start_pfn = start_pfn;

    // Calculate total pages (including holes) and present pages (excluding holes)
    calculate_node_totalpages(pgdat, start_pfn, end_pfn);

    // Initialize zone internals
    free_area_init_core(pgdat);
}
```

---

## `free_area_init_core()` — Zone Structure Initialization

This is where `struct zone` and `struct free_area` are initialized:

```c
static void __init free_area_init_core(pg_data_t *pgdat)
{
    for_each_zone(zone) {
        unsigned long size = zone_spanned_pages(zone);
        unsigned long freesize = zone_absent_pages(zone);

        // === Zone Internal Setup ===
        zone_init_internals(zone, j, nid, freesize);

        if (!size)
            continue;

        // === Pageblock Bitmap ===
        setup_usemap(zone);

        // === Mark Zone as Empty ===
        init_currently_empty_zone(zone, zone->zone_start_pfn, size);
    }
}
```

### `zone_init_internals()` — Initialize Zone Metadata

```c
static void __init zone_init_internals(struct zone *zone, int idx,
                                        int nid, unsigned long remaining)
{
    // Zone identification
    zone->managed_pages = remaining;   // Will be reset to 0 later
    zone->name = zone_names[idx];
    zone->zone_pgdat = NODE_DATA(nid);

    // Locking
    spin_lock_init(&zone->lock);

    // Watermarks (set to 0, calculated later by init_per_zone_wmark_min())
    zone->_watermark[WMARK_MIN] = 0;
    zone->_watermark[WMARK_LOW] = 0;
    zone->_watermark[WMARK_HIGH] = 0;

    // Initialize all buddy free lists
    for (order = 0; order < NR_PAGE_ORDERS; order++) {
        struct free_area *area = &zone->free_area[order];

        for (type = 0; type < MIGRATE_TYPES; type++)
            INIT_LIST_HEAD(&area->free_list[type]);

        area->nr_free = 0;
    }

    // LRU lists
    lruvec_init(&zone->lruvec);

    // Per-CPU page cache
    zone_pcp_init(zone);
}
```

### Buddy Free Lists After Initialization

```
zone->free_area[0]:   nr_free=0, free_list[0..5] = empty
zone->free_area[1]:   nr_free=0, free_list[0..5] = empty
zone->free_area[2]:   nr_free=0, free_list[0..5] = empty
...
zone->free_area[10]:  nr_free=0, free_list[0..5] = empty

ALL EMPTY — no pages freed yet. That happens in Phase 4 (memblock_free_all).
```

### `setup_usemap()` — Pageblock Migration Bitmap

```c
static void __init setup_usemap(struct zone *zone)
{
    unsigned long usemapsize = usemap_size(zone_start_pfn, zone_end_pfn);
    zone->pageblock_flags = memblock_alloc(usemapsize, SMP_CACHE_BYTES);
}
```

**Pageblock bitmap:** Stores the migration type for each **pageblock** (typically 2MB = 512 pages):

```
Pageblock 0: MIGRATE_MOVABLE    (2 bits)
Pageblock 1: MIGRATE_UNMOVABLE  (2 bits)
Pageblock 2: MIGRATE_MOVABLE    (2 bits)
...

Each pageblock = 2MB = 512 pages (order-9 block)
4 bits per pageblock = migration type + skip flag
For 4GB: 2048 pageblocks × 4 bits = 1 KB bitmap
```

---

## Data Flow Summary

```
free_area_init()
│
├── arch_zone_limits_init()
│   └── max_zone_pfn[] = zone PFN boundaries
│
├── sparse_init()
│   └── struct page[] allocated + vmemmap mapped
│
├── Zone range calculation
│   └── arch_zone_lowest/highest_possible_pfn[]
│
└── For each NUMA node:
    └── free_area_init_node(nid)
        ├── pgdat->node_id, node_start_pfn
        ├── calculate_node_totalpages()
        └── free_area_init_core()
            ├── zone_init_internals()    ← Initialize zone metadata
            │   ├── free_area[0..10] = empty lists
            │   ├── watermarks = 0
            │   └── lruvec_init()
            ├── setup_usemap()           ← Pageblock bitmap (memblock_alloc)
            └── init_currently_empty_zone()
```

---

## State After free_area_init()

```
For each NUMA node:
  pglist_data:
    .node_id = nid
    .node_start_pfn = first PFN on this node
    .node_present_pages = pages on this node (excluding holes)
    .node_spanned_pages = PFN range (including holes)

  For each zone (DMA, DMA32, NORMAL):
    struct zone:
      .managed_pages = 0 (reset by reset_all_zones_managed_pages later)
      .free_area[0..10] = ALL EMPTY
      .pageblock_flags = allocated bitmap
      .lock = initialized
      .watermarks = {0, 0, 0}
      .per_cpu_pageset = initialized

struct page[]:
  Allocated from memblock
  Mapped in vmemmap
  pfn_to_page() works for all valid PFNs
  page->flags = PageReserved (all pages start as reserved)
```

---

## Key Takeaways

1. **`struct page` arrays are the biggest memblock allocation** — ~1.5% of total RAM
2. **Buddy free lists are empty** — pages are freed in Phase 4 (`memblock_free_all`)
3. **All pages start as `PageReserved`** — cleared when freed to buddy
4. **Zone boundaries come from architecture** — DMA limits, NUMA topology
5. **Pageblock bitmap enables migration** — tracks which 2MB blocks are movable/unmovable
6. **Watermarks are zero** — calculated later when memory is actually available
