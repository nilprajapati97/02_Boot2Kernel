# Phase 19: `rest_init()` — Transition to Userspace

## Overview

`rest_init()` is the final function called by `start_kernel()`. It represents the point where the kernel transitions from single-threaded boot initialization to a fully multi-threaded, scheduled system. After `rest_init()`, `start_kernel()` never returns.

## The Grand Transition

```
start_kernel()
    │
    └── rest_init()          ← Last call in start_kernel()
            │
            ├── kernel_thread(kernel_init)    → PID 1 (future /sbin/init)
            │
            ├── kernel_thread(kthreadd)       → PID 2 (kernel daemon)
            │
            ├── rcu_scheduler_starting()      → Enable RCU scheduling
            │
            └── cpu_startup_entry(CPUHP_ONLINE)  ← NEVER RETURNS
                    │
                    └── do_idle()             → CPU 0 idle loop
```

## What `rest_init()` Does

```c
noinline void __ref rest_init(void)
{
    struct task_struct *tsk;
    int pid;
    
    rcu_scheduler_starting();
    
    // Spawn kernel_init (future PID 1):
    pid = kernel_thread(kernel_init, NULL, CLONE_FS);
    rcu_read_lock();
    tsk = find_task_by_pid_ns(pid, &init_pid_ns);
    set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));
    rcu_read_unlock();
    
    numa_default_policy();
    
    // Spawn kthreadd (PID 2):
    pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
    rcu_read_lock();
    kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
    rcu_read_unlock();
    
    // Enable kernel thread creation:
    complete(&kthreadd_done);
    
    // CPU 0 becomes the idle thread:
    init_idle_bootup_task(current);
    schedule_preempt_disabled();
    cpu_startup_entry(CPUHP_ONLINE);
    // NEVER REACHED
}
```

## Execution Order

| # | Function | Description |
|---|----------|-------------|
| 1 | [`rcu_scheduler_starting()`](rest_init/rcu_scheduler_starting/README.md) | Enable RCU preemption |
| 2 | `kernel_thread(kernel_init)` | Spawn PID 1 |
| 3 | `kernel_thread(kthreadd)` | Spawn PID 2 |
| 4 | [`cpu_startup_entry()`](rest_init/cpu_startup_entry/README.md) | CPU 0 idle loop |

## Documents in This Phase

- [rest_init/README.md](rest_init/README.md) — rest_init() detailed walkthrough
- [kernel_init/README.md](kernel_init/README.md) — PID 1 path to /sbin/init
- [kthreadd/README.md](kthreadd/README.md) — PID 2 kernel thread daemon
