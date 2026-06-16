# Layer 7 — Zones, Nodes & `struct page` Initialization

[← Layer 6: Page Tables & Paging Init](06_Page_Tables_and_Paging_Init.md) | **Layer 7** | [Layer 8: Buddy Allocator Handoff →](08_Buddy_Allocator_Handoff.md)

---

## What This Layer Covers

The linear map exists and all physical RAM is accessible via virtual addresses. But
the buddy page allocator cannot work yet — it needs **metadata** for every page frame
in the system. This layer explains how the kernel creates the organizational infrastructure
for memory management: NUMA nodes (`pg_data_t`), memory zones (`struct zone`), and
per-page metadata (`struct page`). After this layer, the stage is set for the buddy
allocator handoff.

---

## 1. The Three Levels of Memory Organization

Linux organizes physical memory in a three-level hierarchy:

```
┌─────────────────────────────────────────────────────────────────────┐
│                                                                     │
│   Level 1: NUMA Nodes (pg_data_t)                                   │
│   ═══════════════════════════════                                   │
│   Each physical memory bank connected to a CPU gets its own node.   │
│   Most ARM64 boards have 1 node (UMA). Large servers may have 2-4.  │
│                                                                     │
│   ┌────────────────────────────────────────────────────────────┐     │
│   │  Node 0 (pg_data_t)                                       │     │
│   │                                                            │     │
│   │  Level 2: Zones (struct zone)                              │     │
│   │  ════════════════════════════                              │     │
│   │  Memory is divided into zones based on physical address    │     │
│   │  constraints (DMA capabilities).                           │     │
│   │                                                            │     │
│   │  ┌──────────┐ ┌──────────────┐ ┌───────────────┐          │     │
│   │  │ ZONE_DMA │ │ ZONE_DMA32   │ │ ZONE_NORMAL   │          │     │
│   │  │ (0-1GB)  │ │ (0-4GB)      │ │ (>4GB)        │          │     │
│   │  │          │ │              │ │               │          │     │
│   │  │ Level 3: │ │ Level 3:     │ │ Level 3:      │          │     │
│   │  │ Pages    │ │ Pages        │ │ Pages         │          │     │
│   │  │ struct   │ │ struct page  │ │ struct page   │          │     │
│   │  │ page[]   │ │ array[]      │ │ array[]       │          │     │
│   │  └──────────┘ └──────────────┘ └───────────────┘          │     │
│   └────────────────────────────────────────────────────────────┘     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. `bootmem_init()` — NUMA and Zone Boundaries

**Source**: `arch/arm64/mm/init.c`

Called from `setup_arch()`, this function establishes the NUMA topology and computes
zone boundaries:

```
bootmem_init()
    │
    ├──► Compute global PFN range
    │       min = PFN_UP(memblock_start_of_DRAM())    // First usable PFN
    │       max = PFN_DOWN(memblock_end_of_DRAM())    // Last usable PFN
    │       max_pfn = max_low_pfn = max
    │       min_low_pfn = min
    │
    │       PFN = Page Frame Number = physical_address / PAGE_SIZE
    │       Example: PA 0x4000_0000 → PFN 0x40000 (with 4KB pages)
    │
    ├──► arch_numa_init()
    │       Detect NUMA topology:
    │       ├─ From Device Tree (if DT has distance-map/numa nodes)
    │       └─ From ACPI SRAT table (if ACPI boot)
    │       Assigns each memblock region to a NUMA node.
    │       Most embedded ARM64 boards: 1 node (UMA).
    │
    ├──► kvm_hyp_reserve()
    │       Reserve memory for KVM hypervisor (if enabled).
    │
    ├──► dma_limits_init()
    │       Compute the physical address boundaries for DMA zones:
    │       ┌────────────────────────────────────────────────────┐
    │       │  ZONE_DMA:   0 → arm64_dma_phys_limit              │
    │       │              Typically 0 → 1 GB (30-bit DMA)        │
    │       │              For devices with limited DMA address   │
    │       │              range (e.g., some USB controllers).    │
    │       │                                                    │
    │       │  ZONE_DMA32: 0 → 4 GB (32-bit DMA)                 │
    │       │              For devices that can DMA to the first  │
    │       │              4 GB of physical address space.         │
    │       │                                                    │
    │       │  ZONE_NORMAL: Everything above ZONE_DMA32           │
    │       │              No DMA restrictions.                   │
    │       └────────────────────────────────────────────────────┘
    │
    ├──► dma_contiguous_reserve(arm64_dma_phys_limit)
    │       Reserve CMA (Contiguous Memory Allocator) pool.
    │       CMA provides large contiguous physical allocations
    │       for DMA. Default size: typically 64 MB.
    │       Uses memblock_reserve() internally.
    │
    ├──► arch_reserve_crashkernel()
    │       If "crashkernel=256M" on cmdline: reserve memory
    │       for the kdump crash kernel.
    │
    └──► memblock_dump_all()
            Debug output: print all memblock regions to dmesg.
            "MEMBLOCK memory: ..." and "MEMBLOCK reserved: ..."
```

---

## 3. `mm_core_init_early()` → `free_area_init()`

**Source**: `mm/mm_init.c`

After `setup_arch()` returns to `start_kernel()`, the next major memory call is:

```
start_kernel()
    └──► mm_core_init_early()
         ├──► hugetlb_cma_reserve()    // Reserve CMA for huge pages
         ├──► hugetlb_bootmem_alloc()  // Allocate huge page pool
         └──► free_area_init()  ★       // THE MAIN EVENT
```

### 3.1 `free_area_init()` — The Master Setup Function

```
free_area_init()                                    [mm/mm_init.c]
    │
    ├──► arch_zone_limits_init(max_zone_pfn)
    │       Get the maximum PFN for each zone type.
    │       ARM64 sets:
    │         max_zone_pfn[ZONE_DMA]    = PFN of arm64_dma_phys_limit
    │         max_zone_pfn[ZONE_DMA32]  = PFN of 4 GB
    │         max_zone_pfn[ZONE_NORMAL] = max_pfn
    │
    ├──► sparse_init()
    │       Initialize sparsemem sections.
    │       ARM64 uses CONFIG_SPARSEMEM_VMEMMAP.
    │       Allocates section metadata from memblock.
    │       Sets up the vmemmap (virtual mem_map) mapping.
    │
    ├──► Compute zone PFN ranges
    │       arch_zone_lowest_possible_pfn[zone]  = where each zone starts
    │       arch_zone_highest_possible_pfn[zone] = where each zone ends
    │
    │       ┌──────────────────────────────────────────────────┐
    │       │  Example (2 GB DRAM starting at 0x4000_0000):    │
    │       │                                                  │
    │       │  ZONE_DMA:     PFN 0x40000 → PFN 0x40000        │
    │       │                (0 bytes — DMA limit == DRAM base)│
    │       │                                                  │
    │       │  ZONE_DMA32:   PFN 0x40000 → PFN 0x100000       │
    │       │                (0x4000_0000 → 0x1_0000_0000)     │
    │       │                (3 GB in this zone)               │
    │       │                                                  │
    │       │  ZONE_NORMAL:  PFN 0x100000 → PFN 0x140000      │
    │       │                (remaining above 4 GB if any)     │
    │       └──────────────────────────────────────────────────┘
    │
    ├──► find_zone_movable_pfns_for_nodes()
    │       If "kernelcore=" or "movablecore=" is on cmdline:
    │       compute ZONE_MOVABLE boundaries (for memory hotplug).
    │
    ├──► Print zone ranges (dmesg output):
    │       "Zone ranges:"
    │       "  DMA      [mem 0x40000000-0x....]"
    │       "  DMA32    [mem 0x...-0x...]"
    │       "  Normal   [mem 0x...-0x...]"
    │
    ├──► set_pageblock_order()
    │       Set the size of pageblocks (for migration types).
    │       Default: MAX_PAGE_ORDER (order-10 = 4 MB with 4KB pages)
    │
    ├──► For each NUMA node with memory:
    │       free_area_init_node(nid)     ★
    │       (Explained in Section 4 below)
    │
    └──► memmap_init()                   ★
            Initialize struct page for ALL memory.
            (Explained in Section 5 below)
```

---

## 4. `free_area_init_node()` — Per-Node Initialization

For each NUMA node (most ARM64: just node 0):

```
free_area_init_node(nid)
    │
    ├──► get_pfn_range_for_nid(nid, &start_pfn, &end_pfn)
    │       Query memblock for this node's PFN range.
    │
    ├──► calculate_node_totalpages(pgdat, start_pfn, end_pfn)
    │       For each zone in the node:
    │       ┌───────────────────────────────────────────────────┐
    │       │  spanned_pages = total PFNs in zone (incl. holes)│
    │       │  absent_pages  = PFNs that are holes (no RAM)    │
    │       │  present_pages = spanned - absent (actual RAM)   │
    │       │  zone_start_pfn = first PFN of the zone          │
    │       └───────────────────────────────────────────────────┘
    │
    ├──► alloc_node_mem_map(pgdat)      [FLATMEM only]
    │       Allocate the struct page array for this node.
    │       Size = present_pages × sizeof(struct page)
    │       Allocated from memblock.
    │
    └──► free_area_init_core(pgdat)
            Initialize each zone's internal structures.
            (Explained below)
```

### 4.1 `free_area_init_core()` — Per-Zone Initialization

```
free_area_init_core(pgdat)
    │
    │   For each zone (ZONE_DMA, ZONE_DMA32, ZONE_NORMAL, ZONE_MOVABLE):
    │
    ├──► zone_init_internals(zone, j, nid, present_pages)
    │       ├── zone->managed_pages = present_pages (initial value)
    │       ├── zone->name = zone_names[j]  ("DMA", "DMA32", "Normal"...)
    │       ├── zone->zone_pgdat = pgdat
    │       ├── spin_lock_init(&zone->lock)
    │       ├── Initialize per-CPU page caches (PCP) = empty
    │       └── zone_pcp_init() — configure PCP batch sizes
    │
    ├──► setup_usemap(zone)
    │       Allocate the pageblock flags bitmap.
    │       One entry per pageblock (typically per 2 MB or 4 MB block).
    │       Tracks migration type (UNMOVABLE, MOVABLE, RECLAIMABLE).
    │       Allocated from memblock.
    │
    └──► init_currently_empty_zone(zone, zone_start_pfn, size)
         └──► zone_init_free_lists(zone)
              │
              │   Initialize the buddy allocator's free lists:
              │
              │   For each order (0, 1, 2, ... MAX_PAGE_ORDER):
              │     For each migration type (UNMOVABLE, MOVABLE, ...):
              │       INIT_LIST_HEAD(&zone->free_area[order].free_list[mt])
              │       zone->free_area[order].nr_free = 0
              │
              │   ALL FREE LISTS ARE EMPTY at this point.
              │   No pages are in the buddy allocator yet!
              └──────────────────────────────────────
```

---

## 5. `memmap_init()` — Initializing `struct page` for All Memory

**Source**: `mm/mm_init.c`

Every physical page frame (4 KB) in the system needs a `struct page` descriptor. This
is where they get initialized:

```
memmap_init()
    │
    │   Iterate all memblock memory regions:
    │   for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid):
    │
    └──► memmap_init_range(size, nid, zone_idx, start_pfn, end_pfn, context)
         │
         │   For each PFN in [start_pfn, end_pfn):
         │
         └──► __init_single_page(page, pfn, zone_idx, nid)
              │
              ├── Set page flags:
              │     set_page_links(page, zone, nid, pfn)
              │     → Encodes zone ID and node ID in page->flags
              │
              ├── Set page type:
              │     page_mapcount_reset(page)
              │
              ├── Set reference count:
              │     init_page_count(page)  → atomic_set(&page->_refcount, 1)
              │     refcount = 1 means "reserved/allocated" at boot
              │
              ├── INIT_LIST_HEAD(&page->lru)
              │     LRU list head — not on any list yet
              │
              └── Set pageblock migratetype:
                    If page is first page of a pageblock:
                      set_pageblock_migratetype(page, MIGRATE_MOVABLE)
                    Default: all pageblocks start as MOVABLE
```

### 5.1 `struct page` — The Per-Page Descriptor

Every 4 KB physical page frame has a corresponding `struct page` (~64 bytes).
For a system with 2 GB of RAM:
- Number of page frames: 2 GB / 4 KB = 524,288
- Size of page array: 524,288 × 64 bytes = **32 MB** (~1.56% of RAM)

```
struct page (simplified)
═══════════════════════

┌─────────────────────────────────────────────────────────────┐
│ flags            Page state: zone ID, node ID, PG_locked,   │
│                  PG_dirty, PG_lru, PG_active, PG_reserved   │
├─────────────────────────────────────────────────────────────┤
│ _refcount        Reference count (atomic)                   │
│                  0 = free, 1+ = in use                      │
├─────────────────────────────────────────────────────────────┤
│ _mapcount        How many page tables map this page          │
│                  -1 = not mapped, 0+ = mapped count         │
├─────────────────────────────────────────────────────────────┤
│ lru / buddy_list Linked list node:                          │
│                  • On LRU list (page cache, anon pages)      │
│                  • On buddy free list (when free)            │
├─────────────────────────────────────────────────────────────┤
│ mapping          Points to address_space (file-backed pages) │
│                  or anon_vma (anonymous pages)                │
├─────────────────────────────────────────────────────────────┤
│ index            Offset within the file or swap slot         │
├─────────────────────────────────────────────────────────────┤
│ private          Used by filesystem or swap code             │
├─────────────────────────────────────────────────────────────┤
│ memcg_data       Memory cgroup association                   │
└─────────────────────────────────────────────────────────────┘

At boot time, every struct page is initialized with:
  flags     = zone + node encoded
  _refcount = 1 (reserved — NOT free)
  _mapcount = -1 (not mapped)
  lru       = empty list head
```

---

## 6. `struct zone` — Zone Data Structure

```
struct zone (simplified)
═══════════════════════

┌──────────────────────────────────────────────────────────────┐
│  name                  "DMA", "DMA32", "Normal", "Movable"   │
├──────────────────────────────────────────────────────────────┤
│  zone_start_pfn        First PFN in this zone                │
├──────────────────────────────────────────────────────────────┤
│  spanned_pages         Total PFN span (including holes)      │
│  present_pages         Actual RAM pages (excluding holes)    │
│  managed_pages ★       Pages managed by buddy allocator      │
│                        (present - reserved, set later)       │
├──────────────────────────────────────────────────────────────┤
│  _watermark[3]         min, low, high watermarks             │
│                        Controls when reclaim starts           │
├──────────────────────────────────────────────────────────────┤
│  lowmem_reserve[]      Pages reserved for lower zones        │
├──────────────────────────────────────────────────────────────┤
│  per_cpu_pageset       Per-CPU page caches (PCP)             │
│                        Fast path for single-page alloc/free  │
├──────────────────────────────────────────────────────────────┤
│  free_area[11]         Buddy allocator free lists ★          │
│  ┌─ free_area[0]       Order 0: single pages (4 KB)          │
│  │  ├─ free_list[UNMOVABLE]                                  │
│  │  ├─ free_list[MOVABLE]                                    │
│  │  ├─ free_list[RECLAIMABLE]                                │
│  │  └─ nr_free = 0                                           │
│  ├─ free_area[1]       Order 1: 2-page blocks (8 KB)         │
│  ├─ ...                                                      │
│  └─ free_area[10]      Order 10: 1024-page blocks (4 MB)     │
│                                                              │
│  ALL EMPTY AT THIS POINT! Pages haven't been freed yet.      │
├──────────────────────────────────────────────────────────────┤
│  lock                  Spinlock protecting free_area          │
├──────────────────────────────────────────────────────────────┤
│  zone_pgdat            Pointer to owning pg_data_t           │
└──────────────────────────────────────────────────────────────┘
```

### 6.1 Page Accounting: Three Types of Pages

```
                    ┌────────────────────────────────┐
                    │       spanned_pages             │
                    │   (total PFN range with holes)  │
                    │                                │
                    │   ┌────────────────────────┐   │
                    │   │    present_pages        │   │
                    │   │ (actual RAM, no holes)  │   │
                    │   │                        │   │
                    │   │  ┌──────────────────┐  │   │
                    │   │  │ managed_pages ★  │  │   │
                    │   │  │(present - kernel │  │   │
                    │   │  │ - reserved - etc)│  │   │
                    │   │  │                  │  │   │
                    │   │  │ THESE are what   │  │   │
                    │   │  │ the buddy alloc  │  │   │
                    │   │  │ manages          │  │   │
                    │   │  └──────────────────┘  │   │
                    │   └────────────────────────┘   │
                    └────────────────────────────────┘

★ managed_pages is set properly in Layer 8 (memblock_free_all)
  Right now it equals present_pages, but will be reset to 0
  and rebuilt as pages are actually freed to the buddy system.
```

---

## 7. `pg_data_t` — Per-Node Data Structure

```
pg_data_t (typedef struct pglist_data)
═════════════════════════════════════

┌──────────────────────────────────────────────────────────────┐
│  node_id                  NUMA node number (0 for UMA)       │
├──────────────────────────────────────────────────────────────┤
│  node_zones[MAX_NR_ZONES] Array of struct zone               │
│                           DMA, DMA32, Normal, Movable, ...   │
├──────────────────────────────────────────────────────────────┤
│  node_zonelists[2]        Fallback zone ordering             │
│                           [0] = ZONELIST_FALLBACK             │
│                           [1] = ZONELIST_NOFALLBACK           │
├──────────────────────────────────────────────────────────────┤
│  nr_zones                 Number of populated zones           │
├──────────────────────────────────────────────────────────────┤
│  node_start_pfn           First PFN in this node             │
│  node_present_pages       Physical pages (excl holes)        │
│  node_spanned_pages       Total PFN span (incl holes)        │
├──────────────────────────────────────────────────────────────┤
│  node_mem_map             Pointer to struct page array       │
│                           (FLATMEM only)                     │
├──────────────────────────────────────────────────────────────┤
│  kswapd                   Pointer to kswapd thread           │
│                           (page reclaim daemon — created later)│
├──────────────────────────────────────────────────────────────┤
│  __lruvec                 LRU lists for page reclaim          │
│                           (active/inactive, anon/file lists) │
├──────────────────────────────────────────────────────────────┤
│  totalreserve_pages       Pages that cannot be allocated      │
│                           (watermark reserves)               │
└──────────────────────────────────────────────────────────────┘
```

---

## 8. Zone Types on ARM64

```
Physical Address Space on a Typical ARM64 Board (6 GB RAM)
══════════════════════════════════════════════════════════

0x0000_0002_8000_0000 ┌────────────────────┐
                      │                    │
                      │   ZONE_NORMAL      │  Above 4 GB
                      │   (no DMA limits)  │  = 2 GB in this zone
                      │                    │
0x0000_0001_0000_0000 ├────────────────────┤ ◄── 4 GB boundary
                      │                    │
                      │   ZONE_DMA32       │  Below 4 GB
                      │   (32-bit DMA)     │  Can be reached by any
                      │                    │  device with 32-bit DMA
                      │                    │
0x0000_0000_4000_0000 ├────────────────────┤ ◄── DRAM base
                      │                    │
                      │   (no ZONE_DMA     │  If DRAM starts above
                      │    on this board)  │  the DMA zone limit,
                      │                    │  ZONE_DMA is empty.
0x0000_0000_0000_0000 └────────────────────┘

Zone Boundaries (computed by dma_limits_init):
  ZONE_DMA:     0 → DMA limit (e.g., 1 GB) — may be empty
  ZONE_DMA32:   0 → 4 GB
  ZONE_NORMAL:  4 GB → max_pfn
  ZONE_MOVABLE: (only if kernelcore= or movablecore= is set)
```

### 8.1 Why Zones Exist

```
┌──────────────────────────────────────────────────────────────────┐
│  Some hardware devices can only DMA to a limited address range: │
│                                                                  │
│  • Old ISA devices: only first 16 MB (ZONE_DMA on x86)          │
│  • 32-bit PCI devices: only first 4 GB (ZONE_DMA32)             │
│  • Modern 64-bit devices: any address (ZONE_NORMAL)              │
│                                                                  │
│  When a driver requests DMA-able memory, the kernel allocates    │
│  from the appropriate zone to guarantee the physical address     │
│  falls within the device's addressable range.                    │
│                                                                  │
│  Normal kernel allocations (kmalloc, alloc_pages) prefer         │
│  ZONE_NORMAL and fall back to DMA32/DMA if NORMAL is empty.     │
└──────────────────────────────────────────────────────────────────┘
```

---

## 9. VMEMMAP: Virtual Memory Map

On ARM64 with `CONFIG_SPARSEMEM_VMEMMAP`, the `struct page` array is not allocated as
a single contiguous block. Instead, it uses a **virtual memory map**:

```
VMEMMAP concept:
═══════════════

The kernel reserves a VA region (VMEMMAP_START → VMEMMAP_END)
where struct page entries are laid out as if they were a simple array:

    vmemmap[pfn] = *(struct page *)(VMEMMAP_START + pfn * sizeof(struct page))

But the underlying physical pages for this array are allocated
on-demand from memblock, and only for PFNs that correspond to
actual RAM.

Benefits:
  • No wasted memory for physical address holes
  • PFN → struct page conversion is just pointer arithmetic
  • struct page → PFN conversion is just pointer subtraction

    pfn_to_page(pfn)  = vmemmap + pfn
    page_to_pfn(page) = page - vmemmap
```

---

## 10. State After Layer 7

```
┌────────────────────────────────────────────────────────────────┐
│              STATE AFTER free_area_init()                       │
│                                                                │
│  NUMA Nodes:                                                   │
│    ✓ pg_data_t initialized for each NUMA node                  │
│    ✓ Node PFN ranges computed                                  │
│                                                                │
│  Zones:                                                        │
│    ✓ struct zone initialized for DMA, DMA32, Normal, Movable   │
│    ✓ Zone PFN boundaries computed                              │
│    ✓ Zone free_area[] lists initialized (ALL EMPTY)            │
│    ✓ Per-CPU page caches (PCP) initialized (empty)             │
│    ✓ Pageblock flags bitmap allocated (from memblock)          │
│                                                                │
│  struct page:                                                  │
│    ✓ Every physical page frame has a struct page                │
│    ✓ flags encode zone ID and node ID                          │
│    ✓ _refcount = 1 (all pages considered "reserved")           │
│    ✓ Pageblock migratetype = MOVABLE (default)                 │
│                                                                │
│  Memory Allocators:                                            │
│    ✓ memblock works (and was used heavily in this layer)        │
│    ✗ Buddy allocator has EMPTY free lists                      │
│    ✗ Slab allocator NOT ready                                  │
│    ✗ vmalloc NOT ready                                         │
│                                                                │
│  ★ KEY INSIGHT:                                                │
│    All the infrastructure is set up, but NO pages are free.     │
│    Every page has refcount=1 (reserved). The buddy free lists   │
│    are all empty. The buddy allocator exists in structure but    │
│    has zero pages to hand out.                                  │
│                                                                │
│  What Comes Next:                                              │
│    → memblock_free_all() (Layer 8): take all unreserved pages  │
│      from memblock, clear their PG_Reserved flag, set           │
│      refcount=0, and INSERT them into the buddy free lists.     │
│      THIS is when the buddy allocator truly comes alive.        │
└────────────────────────────────────────────────────────────────┘
```

---

## Summary: What Happened in Layer 7

| Step | Function | What It Did |
|------|----------|-------------|
| 1 | `bootmem_init()` | NUMA detection, DMA limits, CMA/crashkernel reserves |
| 2 | `free_area_init()` | Master zone/node/page setup |
| 3 | `sparse_init()` | Sparsemem section + vmemmap initialization |
| 4 | `free_area_init_node()` | Per-node: PFN ranges, total pages |
| 5 | `free_area_init_core()` | Per-zone: internals, free list init (empty), pageblock bitmap |
| 6 | `memmap_init()` | Per-page: initialize every `struct page` (refcount=1, zone/node flags) |

**The buddy allocator's infrastructure is fully built but completely empty**. Not a single
page has been freed into it. Layer 8 performs the critical handoff where memblock's
unreserved pages are released into the buddy system.

---

[← Layer 6: Page Tables & Paging Init](06_Page_Tables_and_Paging_Init.md) | **Layer 7** | [Layer 8: Buddy Allocator Handoff →](08_Buddy_Allocator_Handoff.md)
