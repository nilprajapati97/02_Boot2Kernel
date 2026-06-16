# Zone & Node Data Structures — Deep Dive

**Source:** `include/linux/mmzone.h`, `include/linux/mm_types.h`
**Phase:** Initialized in Phase 3, used throughout kernel lifetime
**Purpose:** Core data structures for the memory management subsystem

---

## Overview: The Memory Hierarchy

```
System Memory
├── NUMA Node 0 (pglist_data)
│   ├── ZONE_DMA (struct zone)
│   │   ├── free_area[0] — order-0 pages (4 KB)
│   │   │   ├── free_list[MIGRATE_UNMOVABLE]
│   │   │   ├── free_list[MIGRATE_MOVABLE]
│   │   │   ├── free_list[MIGRATE_RECLAIMABLE]
│   │   │   ├── free_list[MIGRATE_HIGHATOMIC]
│   │   │   └── free_list[MIGRATE_CMA]
│   │   ├── free_area[1] — order-1 pages (8 KB)
│   │   ├── ...
│   │   └── free_area[10] — order-10 pages (4 MB)
│   ├── ZONE_DMA32 (struct zone)
│   └── ZONE_NORMAL (struct zone)
├── NUMA Node 1 (pglist_data)
│   └── ...
└── struct page[] — one per physical page frame (in vmemmap)
```

---

## 1. `struct pglist_data` (pg_data_t) — Per-Node Descriptor

One `pglist_data` exists per NUMA node. On single-node systems, there's just one (node 0).

```c
typedef struct pglist_data {
    // === Zone Array ===
    struct zone node_zones[MAX_NR_ZONES];    // Zones on this node

    // === Zone Lists (for allocation fallback) ===
    struct zonelist node_zonelists[MAX_ZONELISTS]; // Ordered zone lists

    // === Node Boundaries ===
    int node_id;                   // NUMA node ID
    unsigned long node_start_pfn;  // First PFN on this node
    unsigned long node_present_pages;  // Pages present (excluding holes)
    unsigned long node_spanned_pages;  // Total PFN range (including holes)

    // === Page Reclaim ===
    wait_queue_head_t kswapd_wait; // Wait queue for kswapd daemon
    struct task_struct *kswapd;    // kswapd thread for this node
    int kswapd_order;              // Highest order kswapd is reclaiming

    // === Per-CPU Stats ===
    struct per_cpu_nodestat __percpu *per_cpu_nodestats;
    atomic_long_t vm_stat[NR_VM_NODE_STAT_ITEMS]; // Node-level VM stats

    // === LRU (Multi-gen) ===
    struct lruvec __lruvec;        // Node-level LRU lists (MGLRU)

    // === Compaction ===
    unsigned long compact_cached_free_pfn;
    unsigned long compact_cached_migrate_pfn[ASYNC_AND_SYNC];

} pg_data_t;
```

### Key Fields Explained

| Field | Type | Purpose |
|-------|------|---------|
| `node_zones[]` | `struct zone[4]` | DMA, DMA32, NORMAL, MOVABLE zones |
| `node_zonelists[]` | `struct zonelist[2]` | Fallback lists for `__alloc_pages()` |
| `node_start_pfn` | PFN | First page frame on this node |
| `node_present_pages` | count | Actual pages (RAM without holes) |
| `node_spanned_pages` | count | PFN range (may include holes) |
| `kswapd` | task | Background reclaim daemon for this node |

### Zone Fallback Lists

```c
struct zonelist {
    struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1];
};

struct zoneref {
    struct zone *zone;     // Pointer to zone
    int zone_idx;          // Zone index (ZONE_DMA, etc.)
};
```

When allocating memory, the kernel tries zones in fallback order:

```
Allocation request: GFP_KERNEL (wants ZONE_NORMAL)
Fallback list for Node 0:
  1. Node 0, ZONE_NORMAL   ← Try first (local, correct zone)
  2. Node 0, ZONE_DMA32    ← Try next (local, lower zone)
  3. Node 0, ZONE_DMA      ← Try next
  4. Node 1, ZONE_NORMAL   ← Remote node (worse NUMA locality)
  5. Node 1, ZONE_DMA32
  6. Node 1, ZONE_DMA
```

---

## 2. `struct zone` — Per-Zone Allocator State

Each zone manages a range of physical pages with the buddy allocator:

```c
struct zone {
    // === Watermarks (memory pressure thresholds) ===
    unsigned long _watermark[NR_WMARK];
    // WMARK_MIN:  Minimum free pages — below this, only critical allocs
    // WMARK_LOW:  Low watermark — kswapd wakes up
    // WMARK_HIGH: High watermark — kswapd sleeps
    // WMARK_PROMO: Promotion watermark (for NUMA promotion)

    unsigned long watermark_boost;   // Temporary watermark increase

    // === Lowmem Reserve ===
    long lowmem_reserve[MAX_NR_ZONES];
    // Reserved pages per zone to prevent lower zones from being
    // exhausted by higher-zone allocations

    // === Buddy Allocator Free Lists ===
    struct free_area free_area[NR_PAGE_ORDERS];  // Orders 0-10

    // === Zone Boundaries ===
    unsigned long zone_start_pfn;    // First PFN in this zone
    unsigned long spanned_pages;     // Total PFN range (with holes)
    unsigned long present_pages;     // Physically present pages
    atomic_long_t managed_pages;     // Pages managed by buddy allocator

    // === Per-CPU Page Cache ===
    struct per_cpu_pages __percpu *per_cpu_pageset;
    struct per_cpu_zonestat __percpu *per_cpu_zonestats;

    // === Page Reclaim ===
    unsigned long flags;             // Zone flags (RECLAIM_IN_PROGRESS, etc.)

    // === Zone Identification ===
    const char *name;                // "DMA", "DMA32", "Normal", "Movable"
    struct pglist_data *zone_pgdat;  // Parent node

    // === Zone Lock ===
    spinlock_t lock;                 // Protects free_area modifications

    // === Compaction ===
    unsigned int compact_considered;
    unsigned int compact_defer_shift;
    int compact_order_failed;

    // === Padding ===
    // Zones are cache-line aligned to avoid false sharing

} ____cacheline_internodealigned_in_smp;
```

### Watermark Mechanism

```
Pages Available:  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
                  0                                    max
                  │         │           │           │
                  │  WMARK  │  WMARK    │  WMARK    │
                  │  _MIN   │  _LOW     │  _HIGH    │
                  │         │           │           │
                  ▼         ▼           ▼           ▼
Action:        OOM/     kswapd      kswapd       Normal
               direct   wakes up    sleeps      allocation
               reclaim  (async      (enough        OK
               (sync)    reclaim)    memory)
```

**Watermark values** are calculated by `init_per_zone_wmark_min()` based on total managed pages. Typical ratio: MIN=~0.1%, LOW=MIN×1.25, HIGH=MIN×1.5.

---

## 3. `struct free_area` — Buddy System Free Lists

```c
struct free_area {
    struct list_head free_list[MIGRATE_TYPES];  // One list per migration type
    unsigned long nr_free;                       // Total free blocks at this order
};
```

### Migration Types

```c
enum migratetype {
    MIGRATE_UNMOVABLE = 0,    // Kernel allocations (can't be moved)
    MIGRATE_MOVABLE,          // User pages (can be migrated/compacted)
    MIGRATE_RECLAIMABLE,      // Page cache, slab reclaimable
    MIGRATE_HIGHATOMIC,       // Reserved for high-order atomic allocs
    MIGRATE_CMA,              // Contiguous Memory Allocator region
    MIGRATE_ISOLATE,          // Temporarily isolated (for hotplug)
    MIGRATE_TYPES             // Number of types
};
```

### Visual: Buddy Free List Structure

```
zone->free_area[0] (order 0, 4KB pages):
  ├── free_list[UNMOVABLE]:  page_A ↔ page_B ↔ page_C   (3 pages)
  ├── free_list[MOVABLE]:    page_D ↔ page_E             (2 pages)
  ├── free_list[RECLAIMABLE]: (empty)
  ├── free_list[HIGHATOMIC]:  (empty)
  └── free_list[CMA]:         (empty)
  nr_free = 5

zone->free_area[1] (order 1, 8KB = 2 contiguous pages):
  ├── free_list[UNMOVABLE]:  page_pair_X                  (1 pair)
  ├── free_list[MOVABLE]:    page_pair_Y ↔ page_pair_Z   (2 pairs)
  └── ...
  nr_free = 3

...

zone->free_area[10] (order 10, 4MB = 1024 contiguous pages):
  ├── free_list[MOVABLE]:    big_block_1                  (1 block)
  └── ...
  nr_free = 1
```

---

## 4. `struct page` — Per-Physical-Page Metadata

Every physical page (4KB frame) has a `struct page` (64 bytes). This is the most heavily used structure in the kernel.

```c
struct page {
    // === Flags (8 bytes) ===
    unsigned long flags;
    // Encodes: zone, node, section, page type flags
    // Flags: PG_locked, PG_referenced, PG_uptodate, PG_dirty,
    //        PG_lru, PG_active, PG_slab, PG_reserved, PG_compound,
    //        PG_private, PG_writeback, PG_head, PG_swapcache, etc.

    // === Union: Different meanings depending on page state ===
    union {
        struct {
            // --- Free page in buddy allocator ---
            union {
                struct list_head lru;       // Link in buddy free_list
                struct {
                    void *__filler;
                    unsigned int mlock_count;
                };
            };
            struct address_space *mapping;  // NULL for free pages
            pgoff_t index;                  // Used by page cache
            unsigned long private;          // Buddy order (for head of free block)
        };

        struct {
            // --- Slab page ---
            struct slab *slab_cache;        // Owning slab cache
            void *freelist;                 // First free object in slab
            union {
                unsigned long counters;
                struct {
                    unsigned inuse:16;      // Objects in use
                    unsigned objects:15;    // Total objects
                    unsigned frozen:1;      // Frozen (per-CPU owned)
                };
            };
        };

        struct {
            // --- Compound page (head) ---
            unsigned long compound_dtor;    // Destructor index
            unsigned long compound_order;   // Order of compound page
            atomic_t compound_mapcount;
            atomic_t subpages_mapcount;
        };

        struct {
            // --- Page table page ---
            unsigned long _pt_pad_1;
            pgtable_t pmd_huge_pte;
            unsigned long _pt_pad_2;
            spinlock_t ptl;                 // Page table lock
        };
    };

    // === Reference Count ===
    atomic_t _refcount;              // Usage reference count

    // === Map Count ===
    atomic_t _mapcount;              // Number of page table mappings
    // -1 = unmapped, 0 = mapped once, N = mapped N+1 times

    // === Mem CGroup ===
    unsigned long memcg_data;        // Memory cgroup association
};
```

### struct page Layout (64 bytes)

```
Offset  Size  Field
──────  ────  ─────
0       8     flags (zone, node, type flags)
8       16    lru / slab_cache+freelist (union)
24      8     mapping / counters (union)
32      8     index / objects+frozen (union)
40      8     private / compound info (union)
48      4     _refcount
52      4     _mapcount
56      8     memcg_data
══════════════
Total:  64 bytes
```

### Page State Flags (in `page->flags`)

```
Bit   Name              Meaning
───   ────              ───────
0     PG_locked          Page is locked (I/O in progress)
1     PG_referenced      Recently accessed (LRU decision)
2     PG_uptodate        Page content is valid
3     PG_dirty           Page modified, needs writeback
4     PG_lru             Page is on an LRU list
5     PG_active          Page is on active LRU (recently used)
6     PG_workingset      Page is in the working set
7     PG_waiters         Someone is waiting for this page
8     PG_error           I/O error occurred
9     PG_slab            Page is used by SLUB allocator
10    PG_reserved        Page is reserved (not available for buddy)
11    PG_private         Page has private data (filesystems)
12    PG_writeback       Page is being written back to disk
13    PG_head            Head page of a compound page
14    PG_mappedtodisk    Has blocks allocated on-disk
15    PG_reclaim         Page is being reclaimed
16    PG_swapbacked       Page is backed by swap
17    PG_unevictable     Page cannot be evicted (mlocked)
```

### flags Field Encoding (Upper Bits)

The upper bits of `page->flags` encode zone, node, and section information:

```
flags layout (64-bit):
┌────────┬────────┬────────────┬─────────────────────────────┐
│Section │  Node  │   Zone     │      Page type flags        │
│[63:54] │[53:50] │[49:48]     │      [47:0]                 │
└────────┴────────┴────────────┴─────────────────────────────┘

page_zone(page)    = (page->flags >> ZONES_PGSHIFT) & ZONES_MASK
page_to_nid(page)  = (page->flags >> NODES_PGSHIFT) & NODES_MASK
page_to_section(page) = (page->flags >> SECTIONS_PGSHIFT) & SECTIONS_MASK
```

---

## 5. `struct per_cpu_pages` — Per-CPU Page Cache

Each zone has a per-CPU cache to reduce lock contention on the zone lock:

```c
struct per_cpu_pages {
    spinlock_t lock;            // Per-CPU lock (less contention than zone->lock)
    int count;                  // Number of pages in cache
    int high;                   // High watermark — drain excess
    int high_min, high_max;     // Dynamic high watermark range
    int batch;                  // Number of pages to add/remove at once
    short free_count;           // Free factor
    short free_factor;          // How aggressively to free pages

    struct list_head lists[NR_PCP_LISTS]; // Per-migration-type, per-order lists
};
```

### PCP Flow

```
Allocation:
  1. Try per-CPU cache (no zone lock needed)  ← FAST
  2. If empty, refill from zone->free_area    ← Needs zone->lock, batch pages

Free:
  1. Add to per-CPU cache                     ← FAST
  2. If count > high, drain to zone->free_area ← Needs zone->lock, batch pages
```

---

## How Structures Interconnect

```
                    ┌────────────────┐
                    │  pglist_data   │ (one per NUMA node)
                    │  node_id = 0   │
                    │  node_zones[]  │──────────────┐
                    └────────────────┘              │
                                                    │
           ┌────────────────────────────────────────┤
           │                                        │
    ┌──────▼──────────┐                    ┌───────▼──────────┐
    │   struct zone    │                    │   struct zone     │
    │   "DMA32"        │                    │   "Normal"        │
    │   free_area[11]  │                    │   free_area[11]   │
    │   per_cpu_pages  │                    │   per_cpu_pages   │
    └─────────────────┘                    └──────────────────┘
           │                                        │
    ┌──────▼──────────┐                    ┌───────▼──────────┐
    │  free_area[0]    │                    │  free_area[0]     │
    │  free_list[MOV]──┼── page ↔ page     │  free_list[MOV]   │
    │  free_list[UNM]  │                    │  ...              │
    │  nr_free = N     │                    │                   │
    └──────────────────┘                    └───────────────────┘
                │
                ▼
    ┌───────────────────┐
    │   struct page      │  (in vmemmap)
    │   flags = zone|nid │
    │   lru → free_list  │  (linked when free)
    │   _refcount = 0    │  (0 when free in buddy)
    │   private = order  │  (buddy order for head page)
    └───────────────────┘
```

---

## Key Takeaways

1. **`struct page` is the foundation** — every memory operation ultimately reads/writes struct page fields
2. **Union overlays save memory** — the same 64 bytes mean different things for free pages, slab pages, compound pages, etc.
3. **Per-CPU caches reduce contention** — most allocations never touch the zone lock
4. **Migration types prevent fragmentation** — keeping movable and unmovable pages separate
5. **Watermarks drive reclaim** — automatic memory pressure response via kswapd
6. **Zone fallback lists enable flexibility** — GFP_KERNEL can fall back from NORMAL to DMA32 to DMA
7. **Everything is cache-line aligned** — `____cacheline_internodealigned_in_smp` prevents false sharing
