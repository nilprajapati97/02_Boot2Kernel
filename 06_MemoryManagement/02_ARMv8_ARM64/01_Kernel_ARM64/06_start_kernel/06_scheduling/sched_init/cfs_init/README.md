# CFS Initialization — Completely Fair Scheduler

## Purpose

Details the initialization of the CFS (Completely Fair Scheduler) per-CPU data structures.

## CFS Runqueue Structure

```c
struct cfs_rq {
    struct load_weight    load;         // Total load of all tasks
    unsigned int          nr_running;   // Number of runnable CFS tasks
    unsigned int          h_nr_running; // Hierarchical count
    
    u64                   exec_clock;   // Total exec time
    u64                   min_vruntime; // Minimum vruntime of all tasks
    
    struct rb_root_cached tasks_timeline; // Red-black tree of tasks
    struct sched_entity  *curr;          // Currently executing entity
    struct sched_entity  *next;          // Next to run (skip list)
    struct sched_entity  *last;          // Last ran (for buddy heuristic)
    struct sched_entity  *skip;          // Skip this entity
    
    // Bandwidth control (CFS groups):
    struct cfs_bandwidth *tg_cfs_bandwidth_used;
};
```

## The vruntime Red-Black Tree

Tasks are ordered in a self-balancing red-black tree by `vruntime`:

```
         task_B (vruntime=150)
        /                    \
task_A (100)            task_C (200)
                       /            \
                  task_D (175)   task_E (250)
```

The **leftmost node** (smallest vruntime) is the next task to run. CFS caches it in `tasks_timeline.rb_leftmost` for O(1) access.

## vruntime Calculation

When a task runs for `delta` ns:

```c
// Weighted delta: high-weight tasks accumulate slower
vruntime += delta * NICE_0_LOAD / task->load.weight;

// Practical effect:
// nice -20 (highest priority): vruntime accumulates SLOWLY → runs longer
// nice  0  (default):         normal accumulation
// nice +19 (lowest priority): vruntime accumulates FAST → preempted quickly
```

## Sched Entity

Each schedulable unit is a `struct sched_entity`:

```c
struct sched_entity {
    struct load_weight  load;
    struct rb_node      run_node;      // Node in cfs_rq.tasks_timeline
    struct list_head    group_node;
    unsigned int        on_rq;         // Currently on a runqueue?
    
    u64                 exec_start;    // Start time of current run
    u64                 sum_exec_runtime; // Total CPU time used
    u64                 vruntime;      // Virtual runtime
    u64                 prev_sum_exec_runtime;
    
    struct sched_entity *parent;       // For cgroup hierarchies
    struct cfs_rq       *cfs_rq;      // Runqueue this entity is on
};
```

`task_struct` embeds a `sched_entity` as `task_struct.se`.

## Initialization

```c
static void init_cfs_rq(struct cfs_rq *cfs_rq)
{
    cfs_rq->tasks_timeline = RB_ROOT_CACHED;
    cfs_rq->min_vruntime = (u64)(-(1LL << 20));  // Initial large value
    // min_vruntime is advanced as tasks are added
}
```

## Cross-references

- [sched_init parent](../README.md)
- [RT and DL init](../rt_dl_init/README.md)
