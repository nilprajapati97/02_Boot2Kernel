# `debug_objects_early_init()` — Object Lifecycle Tracking Bootstrap

## Purpose

Bootstraps the `CONFIG_DEBUG_OBJECTS` infrastructure using a statically allocated pool of object tracker entries. This infrastructure catches bugs where kernel objects (timers, work items, RCU callbacks, etc.) are used after being freed, freed twice, initialized twice, or activated when already active.

## Source File

`lib/debugobjects.c`

```c
void __init debug_objects_early_init(void)
{
    int i;

    for (i = 0; i < ODEBUG_HASH_SIZE; i++)
        raw_spin_lock_init(&obj_hash[i].lock);

    for (i = 0; i < ODEBUG_POOL_SIZE; i++)
        hlist_add_head(&obj_static_pool[i].node, &obj_pool);
}
```

## How DEBUG_OBJECTS Works

Every tracked kernel object (e.g., `struct timer_list`, `struct work_struct`) has a debug state machine with states:

```
ODEBUG_STATE_NONE
    └─► ODEBUG_STATE_INIT      (after init_timer() etc.)
            └─► ODEBUG_STATE_ACTIVE   (after add_timer() etc.)
                    └─► ODEBUG_STATE_INACTIVE  (after del_timer() etc.)
                            └─► ODEBUG_STATE_DESTROYED
```

When an operation violates this state machine (e.g., activating a destroyed timer), `WARN_ON_ONCE` fires with a full stack trace.

## The Early Bootstrap Problem

The object tracker itself uses a hash table of `struct debug_obj` entries. These need a memory allocator. But `kmalloc()` is not yet available this early in boot. Solution: a static array `obj_static_pool[ODEBUG_POOL_SIZE]` of pre-allocated entries is used until `kmalloc()` becomes available (this switchover happens in `debug_objects_mem_init()`).

## Pre-conditions

- Static arrays `obj_hash[]` and `obj_static_pool[]` are zero-initialized (`.bss` section)

## Post-conditions

- `obj_hash[]` spin locks are initialized
- Static object pool is linked into `obj_pool` free list
- Object tracking is functional for the remainder of early boot

## IRQ State

IRQs on or off — this is pure static data initialization.

## Kconfig Dependencies

- `CONFIG_DEBUG_OBJECTS`: Must be enabled; otherwise this is a no-op stub
- `CONFIG_DEBUG_OBJECTS_TIMERS`: Adds tracking for `struct timer_list`
- `CONFIG_DEBUG_OBJECTS_WORK`: Adds tracking for `struct work_struct`
- `CONFIG_DEBUG_OBJECTS_RCU_HEAD`: Adds tracking for `struct rcu_head`

## Key Data Structures

| Symbol | Type | Purpose |
|--------|------|---------|
| `obj_hash[]` | `struct debug_bucket[]` | Hash table mapping addresses to debug entries |
| `obj_static_pool[]` | `struct debug_obj[]` | Static pre-allocated pool for early boot |
| `obj_pool` | `hlist_head` | Free list of available `debug_obj` entries |

## Cross-references

- [Phase overview](../README.md)
- `debug_objects_mem_init()` — switches to kmalloc pool after MM init (called from `mm_core_init()`)
