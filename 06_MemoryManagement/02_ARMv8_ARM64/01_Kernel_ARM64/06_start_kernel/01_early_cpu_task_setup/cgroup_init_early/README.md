# `cgroup_init_early()` — Root Cgroup Pre-allocation

## Purpose

Pre-allocates and initializes the root `cgroup` structure and the root `css_set`, and calls `cgroup_init_subsys()` for each subsystem that is marked `early_init = true`. This must happen before `local_irq_disable()` and very early in boot because many kernel subsystems reference `cgroup` structures from their very first initialization.

## Source File

`kernel/cgroup/cgroup.c`

## Why So Early?

The cgroup subsystem is pervasive — `task_struct` has a `css_set *cgroups` pointer that must point to a valid `css_set` from the moment the first task (init_task) is used. If cgroup structures don't exist yet, any code that touches `current->cgroups` will access garbage memory.

Additionally, some cgroup controllers (like `perf_event` and `freezer`) declare `early_init = true` because they need to be initialized before other kernel subsystems that depend on them.

## What Is a `css_set`?

A `css_set` (cgroup subsystem state set) is the per-task cgroup membership record. It contains a pointer to each subsystem's per-cgroup state for the task:

```c
struct css_set {
    struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT];
    /* ... */
};
```

`init_task` is initialized to point to `init_css_set`, which points to the root cgroup for all subsystems.

## Key Actions

1. Initializes the root `cgroup_root` (top of the cgroup hierarchy)
2. Initializes `init_css_set` — the css_set for `init_task` and all kernel threads
3. For each subsystem with `early_init = 1`, calls `cgroup_init_subsys()`:
   - Allocates root-level `cgroup_subsys_state` (css)
   - Sets up `css->cgroup` and `css->ss` pointers
   - Calls subsystem's `css_alloc()` and `css_online()` callbacks

## Pre-conditions

- `init_task` must be accessible (it is — statically allocated)

## Post-conditions

- `init_css_set` is valid and `init_task.cgroups` points to it
- Early-init subsystems have their root css initialized
- `cgroup_root.cgrp` is valid

## IRQ State

IRQs may be on or off — this function uses only static memory.

## Key Data Structures

| Symbol | Type | Purpose |
|--------|------|---------|
| `cgrp_dfl_root` | `struct cgroup_root` | Default (v2) cgroup hierarchy root |
| `init_css_set` | `struct css_set` | cgroup membership for init_task and kthreads |
| `cgroup_subsys[]` | `struct cgroup_subsys *[]` | Array of all registered subsystem descriptors |

## Kconfig Dependencies

- `CONFIG_CGROUPS`: Required; if not set, stub is used
- `CONFIG_CGROUP_SCHED`, `CONFIG_MEMCG`, etc.: Individual controllers

## Cross-references

- [Phase overview](../README.md)
- `cgroup_init()` — full cgroup v1/v2 init: [../../18_cgroups_control/cgroup_init/README.md](../../18_cgroups_control/cgroup_init/README.md)
