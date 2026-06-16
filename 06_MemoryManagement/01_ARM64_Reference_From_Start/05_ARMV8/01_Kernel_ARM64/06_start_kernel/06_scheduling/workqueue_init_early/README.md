# `workqueue_init_early()` — Early Workqueue Setup

## Purpose

Creates the kernel's global workqueues in "early mode" before SMP is initialized. Workqueues provide a generic mechanism to defer and execute work in kernel thread context. After `workqueue_init_early()`, code can queue work items that will be executed once worker threads are created (later in `workqueue_init()`).

## Source File

`kernel/workqueue.c`

## What is a Workqueue?

A workqueue is a set of kernel threads that execute submitted work items:

```c
// Submit work to run in kernel thread context:
struct work_struct my_work;
INIT_WORK(&my_work, my_work_handler);
schedule_work(&my_work);  // Executes in worker thread later

// Or with delay:
schedule_delayed_work(&my_work, msecs_to_jiffies(100));
```

This is useful when you need to do work that:
- Cannot be done in interrupt context (might sleep)
- Should not block the current thread
- Can be deferred

## Early vs Full Workqueue Init

### `workqueue_init_early()` (this function)

- Creates workqueue objects (`struct workqueue_struct`)
- Creates `pwq` (per-workqueue per-CPU structures)
- Does **not** create worker threads yet (no SMP)
- Queued work is not executed yet

### `workqueue_init()` (later, in `kernel_init_freeable()`)

- Creates actual worker kernel threads per CPU
- Queued work starts executing
- Called after SMP bringup

## Global Workqueues Created

| Workqueue | Usage |
|-----------|-------|
| `system_wq` | General purpose |
| `system_highpri_wq` | High priority work |
| `system_long_wq` | Long-running work (won't be mistaken for a stuck task) |
| `system_unbound_wq` | Not bound to any CPU |
| `system_freezable_wq` | Frozen during suspend |
| `system_power_efficient_wq` | Prefer power efficiency |
| `system_freezable_power_efficient_wq` | Both frozen + power-efficient |

## Work Execution Model

```
schedule_work(work)
    → insert work into workqueue's pending list
    
Worker thread (kworker/N:M):
    → woken up
    → pick work item from list
    → call work->func(work)
    → go back to sleep
```

## Concurrency Management

Modern workqueues use **concurrency-managed worker pools**:
- Tracks how many workers are currently running on each CPU
- Creates new workers dynamically if all are blocked
- Destroys idle workers after a timeout

This avoids the pitfall of "thread-per-work-item" while still allowing parallelism.

## Pre-conditions

- `kmalloc()` available
- Per-CPU areas set up

## Post-conditions

- Global workqueue objects created
- Work can be queued (but not yet executed)
- `schedule_work()` is safe to call

## Cross-references

- [Phase overview](../README.md)
- `workqueue_init()` (full): called in Phase 15 after SMP
