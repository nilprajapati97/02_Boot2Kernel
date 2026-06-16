# `mm_core_init()` — Core Memory Management Initialization

## Purpose

The central memory management initialization function. Transitions the kernel from `memblock`-only allocation to a fully operational buddy allocator with zone-based page management, slab allocator, and `kmalloc`. This is the most critical memory initialization step.

## Source File

`mm/mm_init.c`

## What `mm_core_init()` Does (Call Sequence)

```c
void __init mm_core_init(void)
{
    build_all_zonelists(NULL);     // Build per-NUMA-node zone lists
    page_alloc_init_cpuhp();       // Register CPU hotplug callbacks
    pr_notice("Kernel command line: %s\n", saved_command_line);
    jump_label_init_ro();          // Finalize jump labels (mark RO)
    parse_early_param();           // (no-op if already called)
    
    after_dashes = parse_args(...);// Parse kernel params (second call)
    
    setup_log_buf(0);             // Finalize log buffer

    vfs_caches_init_early();      // Init early dentry/inode (if not done)
    sort_main_extable();          // Sort exception table

    trap_init();                   // Set up IDT
    
    mm_init();                    // The big one: buddy allocator init
    
    ftrace_init();                 // Initialize function tracing
    
    // Initialize early init infrastructure
    early_trace_init();
}
```

Note: The exact call sequence is in `start_kernel()` — `mm_core_init()` contains a subset. Check `init/main.c` for precise ordering.

## The `mm_init()` Sub-function

`mm_init()` is the heart:

```c
static void __init mm_init(void)
{
    page_ext_init_flatmem();     // Early per-page extended data
    init_mem_debugging_and_hardening(); // KASAN, page poisoning setup
    kfence_alloc_pool();         // KFENCE object pool
    report_meminit();            // Print memory init options
    kmsan_init_shadow();         // KMSAN shadow memory
    stack_depot_early_init();    // Stack trace depot
    mem_init();                  // Free memblock to buddy allocator!
    mem_init_print_info();       // Print memory layout info
    kmem_cache_init();           // Initialize SLAB/SLUB allocator
    kmemleak_init();             // Memory leak detection
    pgtable_init();              // Page table caches
    debug_objects_mem_init();    // Debug object tracker
    vmalloc_init();              // Virtual memory area allocator
    /* ... more ... */
}
```

## The `mem_init()` Transition

`mem_init()` is the key transition point — it frees all `memblock`-managed free memory to the buddy allocator:

```
Before mem_init():
    - All physical memory tracked by memblock
    - Allocation: memblock_alloc() only
    - Free pages: tracked as memblock "free" regions

After mem_init():
    - Free pages moved to buddy allocator free lists
    - __free_pages() / alloc_pages() now work
    - memblock regions remain for reserved memory bookkeeping
```

## The Buddy Allocator

After `mem_init()`, memory is organized by **zones** and **orders**:

```
ZONE_DMA (0–16MB on x86):   free_area[0..10]
ZONE_DMA32 (0–4GB):         free_area[0..10]
ZONE_NORMAL (above 4GB):    free_area[0..10]
```

Each `free_area[n]` holds pages in groups of `2^n`:

```
free_area[0] → list of individual pages (4KB)
free_area[1] → list of pairs    (8KB)
free_area[2] → list of quads    (16KB)
...
free_area[10]→ list of 1024-page groups (4MB)
```

## Sub-topics

- [mem_init — freeing memblock](mem_init/README.md)
- [kmem_cache_init — slab allocator](kmem_cache_init/README.md)
- [page_ext_init — per-page extensions](page_ext_init/README.md)

## Pre-conditions

- `setup_arch()` has populated `memblock`
- Per-CPU areas set up

## Post-conditions

- `alloc_pages()`, `__get_free_pages()`: functional
- `kmalloc()`, `kzalloc()`, `kfree()`: functional
- `vmalloc()`: functional
- Memory zones fully initialized

## Cross-references

- [Phase overview](../README.md)
