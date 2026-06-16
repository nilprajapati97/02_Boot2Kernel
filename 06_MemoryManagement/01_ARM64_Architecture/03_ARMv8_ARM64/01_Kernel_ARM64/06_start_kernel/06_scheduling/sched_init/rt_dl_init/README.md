# RT and DL Scheduling Classes

## RT: Real-Time Scheduler (`SCHED_FIFO` / `SCHED_RR`)

### Purpose

Real-time tasks have strict priority over CFS tasks. They are used for time-sensitive operations like audio processing, network drivers, and real-time control systems.

### `rt_rq` Structure

```c
struct rt_rq {
    struct rt_prio_array  active;     // Bitmap + lists per priority level
    unsigned int          rt_nr_running;
    unsigned int          rr_nr_running; // RR tasks
    
    int                   rt_queued;  // Is this rq queued for balancing?
    
    // RT bandwidth throttling:
    int                   rt_throttled;
    u64                   rt_time;    // Time used in current period
    u64                   rt_runtime; // Allowed time per period
    raw_spinlock_t        rt_runtime_lock;
};
```

### Priority Bitmap

RT tasks use 100 priority levels (0..99). The `rt_prio_array` has a bitmap of non-empty levels plus a per-level task list:

```c
struct rt_prio_array {
    DECLARE_BITMAP(bitmap, MAX_RT_PRIO + 1); // 100 bits + 1
    struct list_head queue[MAX_RT_PRIO];      // Per-priority task lists
};
```

Finding the highest-priority runnable RT task = find lowest-numbered set bit in `bitmap` = O(1).

### SCHED_FIFO vs SCHED_RR

- `SCHED_FIFO`: Once scheduled, runs until it blocks or yields. No time slice.
- `SCHED_RR`: Has a time quantum (default 100ms). When time expires, goes to back of its priority queue.

### RT Bandwidth Control

To prevent RT tasks from starving CFS tasks:
- Default: RT tasks can use at most 95% of CPU time per 1-second period
- Configured via `/proc/sys/kernel/sched_rt_runtime_us` and `/proc/sys/kernel/sched_rt_period_us`

---

## DL: Deadline Scheduler (`SCHED_DEADLINE`)

### Purpose

The Deadline scheduling class implements EDF (Earliest Deadline First) for tasks with real-time deadline requirements. It's the highest-priority class in the kernel.

### Task Parameters

A `SCHED_DEADLINE` task has three parameters:

```
runtime  = Maximum CPU time needed per period (e.g., 5ms)
deadline = Relative deadline within period (e.g., 10ms)  
period   = Task period (e.g., 10ms)

Example: "I need 5ms of CPU every 10ms, and I must finish within 10ms"
```

### `dl_rq` Structure

```c
struct dl_rq {
    struct rb_root_cached root;     // Tasks ordered by deadline
    unsigned long         dl_nr_running;
    
    // Bandwidth tracking:
    u64                   running_bw;   // Admitted bandwidth sum
    u64                   this_bw;
    u64                   extra_bw;
    u64                   bw_ratio;
};
```

### Admission Control

Before a task can use `SCHED_DEADLINE`, the kernel checks if the requested bandwidth fits:

```
Sum of (runtime/period) for all DL tasks ≤ 1.0 - margin
```

If not, `sched_setattr()` returns `-EBUSY`.

## Cross-references

- [sched_init parent](../README.md)
- [CFS init](../cfs_init/README.md)
