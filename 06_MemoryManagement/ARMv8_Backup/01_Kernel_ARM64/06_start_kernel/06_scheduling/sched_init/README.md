# `sched_init()` — Scheduler Initialization

## Purpose

Initializes the Linux scheduler: allocates and sets up per-CPU runqueues, initializes all scheduling classes (CFS, RT, DL, idle), sets up the initial task (`init_task`), and prepares the scheduler for first use.

## Source File

`kernel/sched/core.c`

## Key Operations

```c
void __init sched_init(void)
{
    // 1. Allocate scheduler statistics (if enabled)
    sched_clock_init();
    
    // 2. Init wait queue heads
    init_waitqueue_head(&rd->migration_wait);
    
    // 3. For each possible CPU:
    for_each_possible_cpu(i) {
        struct rq *rq = cpu_rq(i);
        
        raw_spin_lock_init(&rq->lock);
        rq->nr_running = 0;
        rq->calc_load_active = 0;
        rq->calc_load_update = jiffies + LOAD_FREQ;
        
        // Init CFS runqueue
        init_cfs_rq(&rq->cfs);
        
        // Init RT runqueue  
        init_rt_rq(&rq->rt);
        
        // Init DL runqueue
        init_dl_rq(&rq->dl);
        
        // Set idle task for each CPU
        rq->idle = &init_task;  // boot CPU; others set in cpu_up()
    }
    
    // 4. Make init_task schedulable
    init_task.sched_class = &fair_sched_class;
    set_load_weight(&init_task, false);
    
    // 5. Set current = init_task for boot CPU
    current->__state = TASK_RUNNING;
    
    // 6. Disable preemption until sched is ready
    preempt_disable();
}
```

## The Runqueue (`struct rq`)

The runqueue is the central per-CPU scheduler data structure:

```c
struct rq {
    raw_spinlock_t  lock;
    
    unsigned int    nr_running;    // Total runnable tasks
    
    struct cfs_rq   cfs;           // CFS class data
    struct rt_rq    rt;            // RT class data
    struct dl_rq    dl;            // Deadline class data
    
    struct task_struct *curr;      // Currently running task
    struct task_struct *idle;      // Idle task for this CPU
    struct task_struct *stop;      // Migration/stop task
    
    u64             clock;         // Monotonic rq clock
    u64             clock_task;    // Task clock (excludes steal)
    
    struct load_weight load;       // Total load of this rq
    
    /* Load balancing: */
    struct sched_domain *sd;
    
    /* Statistics: */
    unsigned long   nr_switches;
    /* ... many more fields ... */
};
```

## CFS: Completely Fair Scheduler

CFS (the default scheduler) uses a **red-black tree** ordered by `vruntime` (virtual runtime):

```
cfs_rq.tasks_timeline (rb_tree):
    vruntime=100 (task A)
    vruntime=150 (task B) ← smallest vruntime = next to run
    vruntime=200 (task C)
```

- Task with the smallest `vruntime` is always scheduled next
- `vruntime` accumulates faster for high-priority tasks (to make them preempted sooner)
- `vruntime` accumulates slower for low-priority (nice) tasks (so they run longer)

## Sub-topics

- [Runqueue Initialization](runqueue_init/README.md)
- [CFS Red-Black Tree](cfs_init/README.md)
- [RT and DL Classes](rt_dl_init/README.md)

## Pre-conditions

- `kmalloc()` available (for sched stats)
- Per-CPU areas set up (runqueue is per-CPU)

## Post-conditions

- All per-CPU runqueues initialized
- `init_task` schedulable on boot CPU
- `current` valid
- Preemption disabled (re-enabled after IRQs enabled)

## Cross-references

- [Phase overview](../README.md)
