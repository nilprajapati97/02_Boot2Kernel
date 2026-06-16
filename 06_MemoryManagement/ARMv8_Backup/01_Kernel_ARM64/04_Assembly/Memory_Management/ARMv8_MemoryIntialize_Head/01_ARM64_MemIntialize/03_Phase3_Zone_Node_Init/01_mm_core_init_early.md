# `mm_core_init_early()` — Zone & Node Initialization Orchestrator

**Source:** `mm/mm_init.c` (called from `init/main.c`)
**Phase:** Zone & Node Init (memblock still active)
**Memory Allocator:** Memblock
**Called by:** `start_kernel()`
**Calls:** `hugetlb_cma_reserve()`, `hugetlb_bootmem_alloc()`, `free_area_init()`

---

## What This Function Does

Orchestrates the initialization of memory zone and node data structures. This is the bridge between "memblock knows about RAM" and "zone/node structures are ready for the buddy allocator."

---

## How It Works With Memory

### Memory Allocated

| What | Allocated From | Purpose |
|------|---------------|---------|
| HugeTLB CMA regions | memblock (reserved) | Contiguous memory for huge pages |
| HugeTLB boot pages | memblock (reserved) | Pre-allocated huge page pool |
| struct page arrays | memblock (allocated in `free_area_init`) | Per-page metadata |
| Zone/node structures | memblock (allocated in `free_area_init`) | Zone management data |
| Pageblock bitmaps | memblock (allocated in `free_area_init`) | Migration type tracking |

---

## Step-by-Step Execution

### Step 1: Reserve CMA for Huge Pages

```c
void __init mm_core_init_early(void)
{
    hugetlb_cma_reserve();
```

**HugeTLB CMA** reserves contiguous memory for huge page allocation at runtime:

```c
void __init hugetlb_cma_reserve(void)
{
    // If hugetlb_cma_size is set (via "hugetlb_cma=" boot parameter)
    // Reserve contiguous regions on each NUMA node

    for_each_node_state(nid, N_MEMORY) {
        // Reserve up to hugetlb_cma_size per node
        cma_declare_contiguous_nid(0, size, 0, PAGE_SIZE,
                                   order_per_bit, false,
                                   "hugetlb", &hugetlb_cma[nid], nid);
    }
}
```

**Example:** `hugetlb_cma=1G` reserves 1GB per NUMA node for huge page allocation.

This uses `memblock_reserve()` internally — the CMA region is carved out from available memory.

---

### Step 2: Allocate Huge Page Boot Pool

```c
    hugetlb_bootmem_alloc();
```

If `hugepages=N` boot parameter is set, pre-allocates N huge pages from memblock:

```c
void __init hugetlb_bootmem_alloc(void)
{
    // For each requested huge page:
    for (i = 0; i < nr_hugepages; i++) {
        // Allocate 2MB (or 1GB) from memblock
        page = memblock_alloc_try_nid(huge_page_size, huge_page_size,
                                       0, MEMBLOCK_ALLOC_ACCESSIBLE, nid);
        // Add to hugetlb free list
    }
}
```

**Why pre-allocate?**
- After the buddy allocator is online, finding contiguous 2MB or 1GB regions becomes increasingly difficult due to fragmentation
- Allocating from memblock guarantees contiguous physical memory

---

### Step 3: Initialize Zones and Nodes

```c
    free_area_init();
}
```

**See:** [02_free_area_init.md](02_free_area_init.md) for full details.

**Summary:** This is the heavyweight — it creates all zone and node data structures, allocates the `struct page` array for every physical page, and prepares the buddy allocator's free list infrastructure.

---

## Context Within start_kernel()

```c
// init/main.c
void __init start_kernel(void)
{
    ...
    setup_arch(&command_line);       // Phase 2: memblock, page tables, zones calculated
    ...
    mm_core_init_early();            // Phase 3: THIS FUNCTION
    ...
    setup_per_cpu_areas();           // Per-CPU memory regions
    ...
    mm_core_init();                  // Phase 4: buddy, SLUB, vmalloc online
    ...
}
```

---

## Memory State Transitions

```
BEFORE mm_core_init_early():
  Memblock: Fully configured (RAM added, kernel/DTB/CMA reserved)
  Page tables: Linear map created (all RAM accessible)
  Zones: Boundaries calculated but no zone structures exist
  struct page[]: Does not exist

AFTER mm_core_init_early():
  Memblock: More regions reserved (hugetlb, struct page arrays, zone data)
  Page tables: vmemmap region mapped (for struct page arrays)
  Zones: struct zone initialized (empty free lists)
  struct page[]: Allocated and mapped (one per physical page frame)
```

---

## Key Takeaways

1. **This is a thin orchestrator** — the real work is in `free_area_init()`
2. **HugeTLB pre-allocation is strategic** — grab contiguous memory before fragmentation
3. **After this function, the buddy infrastructure exists** — but the free lists are empty
4. **Memblock is still the only allocator** — all data structures allocated via `memblock_alloc()`
