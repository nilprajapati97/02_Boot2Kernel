# Phase 6: Scheduling Initialization

## Overview

This phase initializes the Linux process scheduler and related infrastructure. After this phase, the kernel has runqueues, scheduling classes (CFS, RT, DL), and task scheduling data structures fully set up — though only the boot CPU's idle thread is currently "running."

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`sched_init()`](sched_init/README.md) | `kernel/sched/core.c` | Main scheduler initialization |
| 2 | [`housekeeping_init()`](housekeeping_init/README.md) | `kernel/sched/isolation.c` | CPU isolation / nohz_full housekeeping |
| 3 | [`workqueue_init_early()`](workqueue_init_early/README.md) | `kernel/workqueue.c` | Early workqueue setup (pre-smp) |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled

## Memory State

- Full `kmalloc` available

## What Gets Set Up

### Scheduler Core (`sched_init`)

- Per-CPU runqueues (`struct rq`) allocated for each possible CPU
- CFS (Completely Fair Scheduler) tree initialized
- RT (Real-Time) and DL (Deadline) scheduling class queues initialized
- `init_task` (PID 0 / swapper) becomes the first scheduled task
- `current` macro now valid (points to `init_task`)
- Scheduler tick (`sched_tick()`) registered (fires after IRQs enabled)

### CPU Isolation (`housekeeping_init`)

- CPUs listed in `isolcpus=` or `nohz_full=` cmdline are configured
- Isolated CPUs won't run kernel housekeeping tasks

### Workqueue Early (`workqueue_init_early`)

- `system_wq` and similar global workqueues created in early mode
- Worker threads are created later in `workqueue_init()` (after SMP)

## The Three Scheduling Classes

```
sched_dl_class     (Deadline: SCHED_DEADLINE)   ← Highest priority
sched_rt_class     (Real-Time: SCHED_FIFO, SCHED_RR)
sched_fair_class   (CFS: SCHED_NORMAL, SCHED_BATCH, SCHED_IDLE) ← Default
sched_idle_class   (Idle: runs when nothing else can)
```

## Key Global: `init_task`

The boot CPU's initial task (`init_task` = PID 0 = "swapper/0") is the only task at this point. All other tasks are spawned later via `fork()`.

## Function Index

- [sched_init/](sched_init/README.md)
- [housekeeping_init/](housekeeping_init/README.md)
- [workqueue_init_early/](workqueue_init_early/README.md)
