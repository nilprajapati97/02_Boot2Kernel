# `kmem_cache_init()` — SLAB/SLUB Allocator Initialization

## Purpose

Initializes the slab allocator (`kmalloc` / `kfree` backend). After this call, kernel code can allocate and free arbitrary-sized objects efficiently, not just whole pages.

## Source File

- SLUB: `mm/slub.c` (default since kernel 2.6.23)
- SLAB: `mm/slab.c` (legacy)
- SLOB: `mm/slob.c` (tiny embedded systems)

## Why Slab Allocation?

The buddy allocator works in units of `2^order` **pages** (minimum 4KB). Most kernel objects are much smaller (e.g., `task_struct` ~10KB, `inode` ~600B, `dentry` ~200B). Allocating a full page per small object wastes memory.

The slab allocator:
1. Requests pages from the buddy allocator
2. Subdivides them into equal-sized "objects"
3. Tracks which objects are free/used
4. Returns pages to buddy when all objects freed

## SLUB Architecture

SLUB (the current default) uses a simplified design:

```
kmem_cache (per object type):
  ├── cpu_slab (per-CPU):
  │    ├── freelist → next free object pointer
  │    ├── page → current slab page
  │    └── partial → list of partially-used pages
  └── node[N] (per-NUMA-node):
       ├── partial → NUMA-local partial slabs
       └── full → (usually not tracked)
```

### Fast Path (per-CPU, no locking needed)

```c
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gflags)
{
    // 1. Load per-CPU freelist pointer
    void **freelist = this_cpu_ptr(s->cpu_slab)->freelist;
    
    // 2. Pop first free object (single pointer dereference)
    void *object = *freelist;
    this_cpu_ptr(s->cpu_slab)->freelist = *(void **)object;
    
    return object;
    // No locks taken in fast path!
}
```

### kmalloc Cache Sizes

`kmalloc()` uses a set of generic slab caches:

| Size | Cache Name |
|------|------------|
| 8B | `kmalloc-8` |
| 16B | `kmalloc-16` |
| 32B | `kmalloc-32` |
| 64B | `kmalloc-64` |
| 96B | `kmalloc-96` |
| 128B | `kmalloc-128` |
| ... | ... |
| 8MB | `kmalloc-8388608` |

## Two-Phase Initialization

SLUB has a two-phase init because the slab allocator needs to allocate its own metadata structures:

### Phase 1: `kmem_cache_init()`

```c
void __init kmem_cache_init(void)
{
    // 1. Create kmem_cache for kmem_cache objects (self-bootstrapping!)
    kmem_cache = &boot_kmem_cache;
    
    // 2. Create kmem_cache_node caches
    create_boot_cache(&boot_kmem_cache_node, ...);
    
    // 3. Finalize the self-referential kmem_cache
    bootstrap(&boot_kmem_cache);
    
    // 4. Create all kmalloc-N generic caches
    create_kmalloc_caches(0);
    
    slab_state = UP;  // Slab allocator is ready
}
```

### Phase 2: `kmem_cache_init_late()`

```c
void __init kmem_cache_init_late(void)
{
    // Enable per-CPU partial lists (optimization)
    // Finalize NUMA-aware allocation
    slab_state = FULL;
}
```

## Pre-conditions

- Buddy allocator operational (`mem_init()` complete)
- Per-CPU areas set up

## Post-conditions

- `kmalloc(size, GFP_KERNEL)`: functional
- `kzalloc()`, `krealloc()`, `kfree()`: functional
- `kmem_cache_create()`: functional (can create custom caches)

## Inspecting Slabs at Runtime

```bash
# View all slab caches:
cat /proc/slabinfo

# Or with slabtop:
slabtop
```

## Cross-references

- [mm_core_init parent](../README.md)
- `kmem_cache_init_late()`: [../../kmem_cache_init_late/README.md](../../kmem_cache_init_late/README.md)
