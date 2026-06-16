# Runqueue Initialization — Per-CPU Scheduler Data

## Purpose

Details of how each CPU's runqueue (`struct rq`) is initialized during `sched_init()`.

## Per-CPU Runqueue Layout

Each CPU gets one `struct rq` embedded in the per-CPU `.data` section:

```c
DEFINE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

#define cpu_rq(cpu)  (&per_cpu(runqueues, (cpu)))
#define this_rq()    this_cpu_ptr(&runqueues)
```

Accessing it: `cpu_rq(cpu)` uses the per-CPU offset for zero-cost access on the local CPU.

## Boot CPU vs Other CPUs

At `sched_init()` time, only the boot CPU's runqueue is "active":

```
Boot CPU (CPU 0):
    rq->idle    = &init_task       ← PID 0 / swapper/0
    rq->curr    = &init_task       ← Currently running
    rq->nr_running = 0             ← init_task not counted as "runnable"

Other CPUs (1..N):
    rq->idle    = NULL             ← Set in cpu_up() → idle_init()
    rq->curr    = NULL
    rq->nr_running = 0
```

## Load Accounting

Each runqueue tracks load for the load balancer:

```c
// Per-rq load (sum of weights of all runnable tasks):
struct load_weight {
    unsigned long  weight;    // Sum of task weights (priority-based)
    u32            inv_weight;// Precomputed inverse for division
};

// PELT (Per-Entity Load Tracking):
struct sched_avg {
    u64             last_update_time;
    u64             load_sum;      // Accumulated load
    u64             runnable_sum;
    u32             util_sum;      // CPU utilization
    u32             period_contrib;
    unsigned long   load_avg;      // EWMA of load
    unsigned long   runnable_avg;
    unsigned long   util_avg;      // EWMA of utilization (0..1024)
};
```

## Migration Infrastructure

Each runqueue has a `migration_wait` waitqueue and associated `stop_task` (the highest-priority task on the system, used by `stop_machine()`).

## Cross-references

- [sched_init parent](../README.md)
- [CFS init](../cfs_init/README.md)
