# `mm_core_init()` — Bringing All Allocators Online

**Source:** `mm/mm_init.c` lines 2710–2772
**Phase:** Allocators Online — The final phase of memory initialization
**Memory Allocator:** Transition from memblock to buddy + SLUB + vmalloc
**Called by:** `start_kernel()`
**Calls:** `memblock_free_all()`, `kmem_cache_init()`, `vmalloc_init()`, and many more

---

## What This Function Does

`mm_core_init()` is the **grand transition function** — it takes the system from the memblock boot allocator to the full production memory management stack:

1. **Frees** all non-reserved memblock memory to the buddy allocator
2. **Initializes** the SLUB slab allocator (for `kmalloc()`)
3. **Initializes** vmalloc (for large virtual allocations)
4. **Configures** watermarks, page compaction, and other runtime systems

After this function, `kmalloc()`, `get_free_pages()`, `vmalloc()`, and `kfree()` all work.

---

## How It Works With Memory

### Allocator Timeline

```
BEFORE mm_core_init():
  memblock       → ACTIVE (sole allocator)
  buddy/zones    → INITIALIZED but EMPTY (no free pages)
  SLUB/kmalloc   → NOT AVAILABLE
  vmalloc        → NOT AVAILABLE

DURING mm_core_init():
  memblock       → RETIRING (freeing pages to buddy)
  buddy/zones    → RECEIVING pages
  SLUB/kmalloc   → BOOTSTRAPPING
  vmalloc        → INITIALIZING

AFTER mm_core_init():
  memblock       → RETIRED (code freed, data in .init section)
  buddy/zones    → ACTIVE (primary page allocator)
  SLUB/kmalloc   → ACTIVE (object allocator)
  vmalloc        → ACTIVE (virtual memory allocator)
```

---

## Step-by-Step Execution

### Step 1: Report Memory Info

```c
void __init mm_core_init(void)
{
    report_meminit();  // Log memory sanitization status (zero-fill, etc.)
```

---

### Step 2: Initialize kmem_cache Infrastructure

```c
    kmem_cache_init();
```

**See:** [04_kmem_cache_init_slub.md](04_kmem_cache_init_slub.md) for full details.

**Summary:** Bootstraps the SLUB slab allocator. After this:
- `kmem_cache_create()` works
- `kmalloc()` works for common sizes (8, 16, 32, ..., 8192 bytes)

---

### Step 3: Housekeeping Initialization

```c
    housekeeping_init();  // Mark CPUs as housekeeping vs. isolated

    page_ext_init_flatmem();  // Extended page info for flatmem (not ARM64)
```

---

### Step 4: Initialize Memory Debugging

```c
    init_debug_pagealloc();  // PAGE_ALLOC debug: poison freed pages

    init_mem_debugging_and_hardening();  // KASAN, stack hardening
```

`init_debug_pagealloc()` enables page poisoning — when pages are freed to buddy, they're filled with a poison pattern (0xAA). When allocated, they're checked for corruption.

---

### Step 5: Initialize kfence

```c
    kfence_alloc_pool_and_metadata();  // KFENCE: sampled memory error detector
```

KFENCE (Kernel Electric Fence) pre-allocates a pool of guard pages for sampling-based detection of use-after-free and out-of-bounds bugs.

---

### Step 6: FREE ALL MEMBLOCK MEMORY TO BUDDY

```c
    memblock_free_all();
```

**See:** [02_memblock_free_all.md](02_memblock_free_all.md) for full details.

**This is THE transition point.** All memory not reserved by memblock is freed to the buddy allocator. After this:
- `alloc_pages()` returns pages from the buddy free lists
- Zone watermarks become meaningful
- Memory pressure detection works

---

### Step 7: Report Managed Pages

```c
    mem_init_print_info();
    // Prints: "Memory: 3907432K/4194304K available
    //          (12288K kernel code, 2048K rodata, ...)"
```

---

### Step 8: Initialize Per-CPU Page Caches

```c
    setup_per_cpu_pageset();  // Populate zone->per_cpu_pageset
```

Creates per-CPU page caches for each zone. After this, most page allocations go through the fast per-CPU path without taking the zone lock.

---

### Step 9: Configure Watermarks

```c
    init_per_zone_wmark_min();  // Calculate WMARK_MIN/LOW/HIGH for each zone
```

Watermarks are calculated based on `managed_pages`:

```c
// Simplified formula:
min_free_kbytes = sqrt(lowmem_kbytes × 16);  // Typically 1-4% of RAM
per_zone_min = min_free_kbytes × zone->managed_pages / total_managed;

zone->_watermark[WMARK_MIN] = per_zone_min;
zone->_watermark[WMARK_LOW] = per_zone_min + per_zone_min / 4;
zone->_watermark[WMARK_HIGH] = per_zone_min + per_zone_min / 2;
```

---

### Step 10: Lowmem Reserves

```c
    setup_per_zone_lowmem_reserve();
```

Prevents lower zones (DMA) from being exhausted by allocations targeting higher zones (NORMAL):

```
Example:
  ZONE_DMA has 1 GB
  ZONE_NORMAL requests memory
  Without reserve: NORMAL allocations could consume all DMA pages
  With reserve: DMA keeps lowmem_reserve[NORMAL] pages for DMA-only use
```

---

### Step 11: vmalloc Initialization

```c
    vmalloc_init();
```

**See:** [05_vmalloc_init.md](05_vmalloc_init.md) for full details.

**Summary:** Initializes the virtual memory allocation subsystem. After this, `vmalloc()` and `vmap()` work.

---

### Step 12: Page Compaction & Remaining Init

```c
    compaction_init();    // Initialize page compaction (defragmentation)
    page_alloc_init_cpuhp();  // CPU hotplug callbacks for page allocator
    page_alloc_sysctl_init(); // Register sysctl knobs for tuning
    init_page_ext();      // Extended page info (for debugging features)
}
```

---

## Complete Call Sequence

```c
void __init mm_core_init(void)
{
    report_meminit();

    // === Phase A: SLUB Bootstrap ===
    kmem_cache_init();              // SLUB slab allocator online

    // === Phase B: Debug & Hardening ===
    init_debug_pagealloc();
    init_mem_debugging_and_hardening();
    kfence_alloc_pool_and_metadata();

    // === Phase C: THE BIG TRANSITION ===
    memblock_free_all();            // Free memblock pages → buddy

    // === Phase D: Runtime Configuration ===
    setup_per_cpu_pageset();        // Per-CPU page caches
    init_per_zone_wmark_min();      // Zone watermarks
    setup_per_zone_lowmem_reserve();// Zone reserves

    // === Phase E: Virtual Memory ===
    vmalloc_init();                 // vmalloc/vmap online

    // === Phase F: Compaction & Sysctl ===
    compaction_init();
    page_alloc_init_cpuhp();
    page_alloc_sysctl_init();
}
```

---

## Memory State After mm_core_init()

```
Buddy Allocator:
  ZONE_DMA:    managed_pages = N, free_area[] populated
  ZONE_DMA32:  managed_pages = M, free_area[] populated
  ZONE_NORMAL: managed_pages = P, free_area[] populated

  Total free pages ≈ total RAM - kernel - DTB - page tables - struct page

  Watermarks:
    WMARK_MIN  = calculated (critical threshold)
    WMARK_LOW  = WMARK_MIN × 1.25 (kswapd wake)
    WMARK_HIGH = WMARK_MIN × 1.5 (kswapd sleep)

SLUB:
  boot_kmem_cache → replaced with proper slab-allocated cache
  kmalloc caches: kmalloc-8 through kmalloc-8k (and DMA variants)

vmalloc:
  vmap_area_list → populated with kernel VMA entries
  VA range ready: [VMALLOC_START, VMALLOC_END)

Available APIs:
  alloc_pages(gfp, order)     ← Buddy allocator
  kmalloc(size, gfp)          ← SLUB slab allocator
  vmalloc(size)               ← Virtual memory allocator
  kfree(ptr)                  ← SLUB free
  vfree(ptr)                  ← vmalloc free
  __get_free_pages(gfp, order) ← Page allocator (returns VA)
```

---

## Key Takeaways

1. **Order matters critically** — SLUB must be bootstrapped before memblock_free_all() because freeing pages needs per-CPU caches which need slab
2. **memblock_free_all() is the transition** — after it, memblock is dead, buddy is alive
3. **Watermarks enable pressure response** — kswapd and direct reclaim kick in automatically
4. **vmalloc comes last** — it depends on both buddy (for page allocation) and SLUB (for metadata)
5. **After mm_core_init(), memory management is complete** — all production allocators are operational
