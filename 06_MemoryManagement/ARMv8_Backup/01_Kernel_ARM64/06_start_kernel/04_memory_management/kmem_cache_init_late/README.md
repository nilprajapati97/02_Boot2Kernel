# `kmem_cache_init_late()` — Finalize Slab Allocator

## Purpose

Completes the slab allocator initialization that could not be done in `kmem_cache_init()`. Specifically enables per-CPU partial slab lists and any NUMA-related optimizations that require a more complete system state.

## Source File

`mm/slub.c`

```c
void __init kmem_cache_init_late(void)
{
    struct kmem_cache *s;
    
    // Enable per-CPU partial lists for all existing caches
    mutex_lock(&slab_mutex);
    list_for_each_entry(s, &slab_caches, list) {
        if (slab_state >= UP)
            __kmem_cache_create(s, s->flags);
    }
    mutex_unlock(&slab_mutex);
    
    slab_state = FULL;
}
```

## What Changes After This Call

| Feature | Before | After |
|---------|--------|-------|
| `kmalloc` | Works (basic) | Fully optimized |
| Per-CPU partial lists | Disabled | Enabled |
| NUMA-aware allocation | Basic | Full |
| `slab_state` | `UP` | `FULL` |

## Why Two-Phase Init?

`kmem_cache_init()` runs before other subsystems (mutex, per-CPU work, etc.) are initialized. It uses a simplified bootstrap mode.

`kmem_cache_init_late()` runs after `mutex_init()` and per-CPU infrastructure, so it can use those facilities.

## The `slab_state` Progression

```
DOWN       → Initial state (no slab)
PARTIAL    → kmem_cache for kmem_cache objects exists
PARTIAL_NODE → Node structures available
UP         → kmalloc() works (basic)
FULL       → All optimizations enabled (after kmem_cache_init_late())
```

## Pre-conditions

- `kmem_cache_init()` complete (`slab_state == UP`)
- CPU hotplug callbacks registered (for per-CPU list setup)

## Post-conditions

- `slab_state == FULL`
- Maximum allocation performance
- Can create per-NUMA-node slab caches

## Cross-references

- [Phase overview](../README.md)
- [kmem_cache_init](mm_core_init/kmem_cache_init/README.md)
