# `debug_objects_early_init()` — Debug Object Lifecycle Tracking

## Overview

| Attribute    | Value                                         |
|-------------|------------------------------------------------|
| **Function** | `debug_objects_early_init(void)`              |
| **Source**   | `lib/debugobjects.c`                          |
| **Config**   | `CONFIG_DEBUG_OBJECTS`                        |
| **Purpose**  | Initialize the object debug tracking system's per-CPU pools and static hash buckets |

---

## Why It Exists

The Linux kernel has **thousands of objects** (timers, work items, mutexes, etc.) that have strict lifecycle requirements:
- A timer must be initialized before `mod_timer()` is called
- A mutex must not be used after it's destroyed
- A work item must not be queued after its queue has been destroyed

Without tracking, these violations cause silent memory corruption. `CONFIG_DEBUG_OBJECTS` adds lifecycle state tracking:

```
ODEBUG_STATE_NONE → INIT → ACTIVE → DESTROYED
```

Any violation (double init, use-after-destroy, etc.) triggers a `WARN_ON_ONCE` with the offending code location.

---

## Internal Deep Dive

### Early Bootstrap Problem

The debug objects system itself needs to track objects, but the slab allocator (`kmalloc`) is not yet available. Solution: use **statically allocated hash buckets**:

```c
// lib/debugobjects.c
#define ODEBUG_HASH_BITS    14
#define ODEBUG_HASH_SIZE    (1 << ODEBUG_HASH_BITS)  // 16384

struct debug_bucket {
    struct hlist_head  list;
    raw_spinlock_t     lock;
};

static struct debug_bucket obj_hash[ODEBUG_HASH_SIZE];  // static! no kmalloc needed
```

`debug_objects_early_init()` initializes the spinlocks in each of the 16384 buckets:

```c
void __init debug_objects_early_init(void)
{
    int i;
    for (i = 0; i < ODEBUG_HASH_SIZE; i++)
        raw_spin_lock_init(&obj_hash[i].lock);

    for (i = 0; i < ODEBUG_POOL_SIZE; i++)
        hlist_add_head(&obj_pool[i].node, &obj_pool_free);
    // ... initialize per-CPU pools
}
```

### Per-CPU Object Pools

To avoid lock contention on the global hash, each CPU gets a small pool of `debug_obj` entries. These are allocated from a static `obj_pool` array initially, then replaced with slab-allocated objects after `kmalloc` becomes available (`debug_objects_mem_init()`).

---

## Runtime Object Tracking

Once initialized, the system tracks every registered object:

```c
// Example: timer debugging
void init_timer_key(struct timer_list *timer, ...) {
    debug_init(&timer->entry, &timer_debug_descr, timer);
    // ↑ calls debug_object_init() → looks up/creates entry in obj_hash
}

void mod_timer(struct timer_list *timer, ...) {
    debug_activate(&timer->entry, &timer_debug_descr, timer);
    // ↑ transitions state to ACTIVE, catches if not INIT'd
}
```

---

## Interview Q&A

### Q1: What is the performance impact of `CONFIG_DEBUG_OBJECTS`?
**A:** Significant. Every `init_timer`, `add_work`, `mutex_init` etc. does a hash lookup in `obj_hash` and a spinlock acquisition. On a 16-core system running a network benchmark, this can reduce throughput by 30-50%. This is why `CONFIG_DEBUG_OBJECTS` is **never enabled in production kernels** — only in debug/development builds. Distributions like RHEL/Ubuntu ship without it.

### Q2: Why use a static hash array instead of a dynamically allocated one?
**A:** Because `debug_objects_early_init()` runs before `mm_core_init()` — the slab allocator (`kmalloc`) is not yet available. Any allocation attempt would crash. The 16384-entry static array costs ~16384 × (8 + 4) = ~200KB of BSS — acceptable for a debug build. After `kmalloc` becomes available, `debug_objects_mem_init()` transitions to heap-allocated per-CPU pools.

### Q3: What is the difference between `debug_objects_early_init()` and `debug_objects_mem_init()`?
**A:** `debug_objects_early_init()` (Phase 1) initializes static structures — no dynamic allocation. `debug_objects_mem_init()` (called after `mm_core_init()`) allocates proper slab caches for `debug_obj` structures and replaces the static pool. Before `mem_init()`, object tracking works but uses pre-allocated static pool entries. Once the pool is exhausted early in boot, new objects can't be tracked until `mem_init()` runs.
