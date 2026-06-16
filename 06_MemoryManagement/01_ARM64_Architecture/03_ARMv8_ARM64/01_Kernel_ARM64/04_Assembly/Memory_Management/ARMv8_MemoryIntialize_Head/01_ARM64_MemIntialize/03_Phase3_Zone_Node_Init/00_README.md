# Phase 3 — Zone & Node Initialization

## Overview

With memblock fully configured and page tables created, the kernel now needs to initialize the **runtime memory management data structures**:

1. **`struct pglist_data`** — per-NUMA-node memory descriptor
2. **`struct zone`** — per-zone allocator state (DMA, DMA32, NORMAL)
3. **`struct free_area[11]`** — buddy allocator free lists (empty at this point)
4. **`struct page`** arrays — one `struct page` for every physical page frame (allocated from memblock)

After this phase, the zone/node infrastructure is ready, but the **buddy allocator free lists are still empty** — no pages have been freed into them yet.

## Call Graph

```
start_kernel()                                    ← init/main.c
└── mm_core_init_early()                          ← mm/mm_init.c
    │
    ├── hugetlb_cma_reserve()                     ← Reserve CMA for huge pages
    ├── hugetlb_bootmem_alloc()                   ← Allocate huge page pools
    │
    └── free_area_init()                          ← mm/mm_init.c:1700
        │
        ├── arch_zone_limits_init(max_zone_pfn)   ← Get zone boundaries
        │
        ├── sparse_init()                         ← mm/sparse.c
        │   ├── Allocate mem_section[] array       ← From memblock
        │   └── sparse_init_nid()                 ← Per-node vmemmap setup
        │       ├── Allocate struct page arrays    ← From memblock
        │       └── Map into vmemmap VA range      ← Virtual mapping
        │
        ├── Zone range calculation loop
        │   └── For each zone type:
        │       ├── arch_zone_lowest_possible_pfn[]
        │       └── arch_zone_highest_possible_pfn[]
        │
        ├── find_zone_movable_pfns_for_nodes()    ← ZONE_MOVABLE boundaries
        │
        └── For each NUMA node:
            └── free_area_init_node()
                ├── calculate_node_totalpages()
                └── free_area_init_core()
                    ├── zone_init_internals()      ← Init zone metadata
                    │   ├── zone->lock = SPIN_LOCK
                    │   ├── zone->_watermark[] = 0
                    │   └── INIT_LIST_HEAD(free_area[].free_list[])
                    ├── setup_usemap()             ← Pageblock flags bitmap
                    └── init_currently_empty_zone() ← Mark zones empty
```

## Memory Allocated in This Phase

| What | Allocated From | Size | Purpose |
|------|---------------|------|---------|
| `struct page[]` | memblock | 64 bytes × total_pages | Per-page metadata for all RAM |
| `mem_section[]` | memblock | Depends on RAM size | SPARSEMEM section descriptors |
| vmemmap mappings | memblock (page tables) | PGD/PUD/PMD/PTE pages | Map struct page into vmemmap VA |
| Pageblock bitmap | memblock | 1 bit per pageblock | Migration type flags |

**Note:** All allocations come from **memblock** — the buddy allocator does not exist yet.

## Documents

| # | Document | Covers |
|---|----------|--------|
| 01 | [mm_core_init_early.md](01_mm_core_init_early.md) | Orchestrator, hugetlb reserves |
| 02 | [free_area_init.md](02_free_area_init.md) | Zone PFN calculation, per-node init |
| 03 | [sparse_init.md](03_sparse_init.md) | SPARSEMEM model, vmemmap mapping |
| 04 | [zone_and_node_structures.md](04_zone_and_node_structures.md) | All data structures explained |

## State After This Phase

```
Zone Data Structures:  INITIALIZED (watermarks=0, managed_pages=0)
Buddy Free Lists:      EMPTY (no pages freed yet)
struct page[]:         ALLOCATED (from memblock, mapped in vmemmap)
Memblock:              STILL ACTIVE (sole allocator)
```
