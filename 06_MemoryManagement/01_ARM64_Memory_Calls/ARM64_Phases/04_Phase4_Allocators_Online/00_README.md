# Phase 4 — Allocators Come Online

## Overview

This is the **final and most critical phase** of memory initialization. The kernel transitions from the boot-time memblock allocator to the runtime allocator stack:

1. **Buddy allocator** — page-level allocation (`alloc_pages()`)
2. **SLUB slab allocator** — object-level allocation (`kmalloc()`, `kmem_cache_alloc()`)
3. **vmalloc** — virtually contiguous allocation (`vmalloc()`, `ioremap()`)

The key event is `memblock_free_all()` which **releases all unreserved memblock pages to the buddy allocator**. After this, memblock is no longer used.

## Call Graph

```
start_kernel()                                    ← init/main.c
└── mm_core_init()                                ← mm/mm_init.c:2710
    │
    ├── arch_mm_preinit()                         ← swiotlb DMA bounce buffers
    ├── build_all_zonelists(NULL)                  ← Build zone fallback lists
    ├── page_alloc_init_cpuhp()                   ← CPU hotplug for page allocator
    │
    ├── memblock_free_all()                       ← *** MEMBLOCK → BUDDY ***
    │   ├── free_unused_memmap()                  ← Free gaps in struct page array
    │   ├── reset_all_zones_managed_pages()       ← managed_pages = 0
    │   └── free_low_memory_core_early()          ← Walk memblock regions
    │       └── for each unreserved region:
    │           └── __free_pages_core()           ← Hand pages to buddy
    │               ├── __ClearPageReserved()     ← Clear reserved flag
    │               ├── set_page_count(p, 0)      ← Mark as free
    │               └── __free_pages_ok()         ← Add to buddy free list
    │                   └── __free_one_page()     ← Buddy merging algorithm
    │
    ├── mem_init()                                ← Mark page allocator available
    │
    ├── kmem_cache_init()                         ← *** SLUB BOOTSTRAP ***
    │   ├── Static boot_kmem_cache (in .data)     ← Chicken-and-egg solution
    │   ├── create_boot_cache(kmem_cache_node)    ← First slab cache
    │   ├── slab_state = PARTIAL
    │   ├── create_boot_cache(kmem_cache)         ← Cache for cache metadata
    │   ├── bootstrap(&boot_kmem_cache)           ← Replace static with real
    │   ├── bootstrap(&boot_kmem_cache_node)
    │   └── create_kmalloc_caches()               ← kmalloc-8, -16, ..., -8192
    │
    ├── vmalloc_init()                            ← *** VMALLOC ***
    │   ├── KMEM_CACHE(vmap_area)                 ← Slab cache for VA descriptors
    │   ├── vmap_init_nodes()                     ← Per-node RB-tree init
    │   ├── Import vmlist entries                 ← Kernel text/data VMAs
    │   └── vmap_init_free_space()                ← Free VA tracking
    │
    ├── kmemleak_init()                           ← Memory leak detector
    ├── pgtable_cache_init()                      ← Page table caches
    ├── mm_cache_init()                           ← mm_struct caches
    └── execmem_init()                            ← Executable memory init
```

## Allocator Transition Timeline

```
memblock_free_all()     kmem_cache_init()     vmalloc_init()
      │                       │                     │
      ▼                       ▼                     ▼
┌──────────┐           ┌──────────┐           ┌──────────┐
│  BUDDY   │ ────────► │  SLUB    │ ────────► │ VMALLOC  │
│  ONLINE  │           │  ONLINE  │           │  ONLINE  │
└──────────┘           └──────────┘           └──────────┘
│                      │                      │
│ alloc_pages()        │ kmalloc()            │ vmalloc()
│ __get_free_pages()   │ kmem_cache_alloc()   │ ioremap()
│ free_pages()         │ kfree()              │ vmap()
```

## Documents

| # | Document | Covers |
|---|----------|--------|
| 01 | [mm_core_init.md](01_mm_core_init.md) | Master orchestrator — full call sequence |
| 02 | [memblock_free_all.md](02_memblock_free_all.md) | THE transition: memblock → buddy |
| 03 | [buddy_allocator.md](03_buddy_allocator.md) | Free/alloc algorithm, merging, splitting |
| 04 | [kmem_cache_init_slub.md](04_kmem_cache_init_slub.md) | SLUB bootstrap, chicken-and-egg problem |
| 05 | [vmalloc_init.md](05_vmalloc_init.md) | VA space management, RB-tree init |
| 06 | [allocator_summary.md](06_allocator_summary.md) | Timeline, address space layout, reference |

## State After This Phase

```
Memblock:        RETIRED (no longer used for allocations)
Buddy:           FULLY OPERATIONAL (all free pages in free_area[] lists)
SLUB:            FULLY OPERATIONAL (kmalloc-8 through kmalloc-8192 ready)
vmalloc:         FULLY OPERATIONAL (VA space tracked in RB-trees)
Runtime:         ALL ALLOCATORS AVAILABLE
```
