# `fork_init()` — Task Structure Cache and Fork Limits

## Purpose

Creates the slab cache for `struct task_struct` (the kernel's representation of every process/thread), sets the maximum number of threads based on available RAM, and initializes the `RLIMIT_NPROC` (max process limit) resource limit.

## Source File

`kernel/fork.c`

```c
void __init fork_init(void)
{
    int i;
    
#ifndef CONFIG_ARCH_TASK_STRUCT_ALLOCATOR
    // Create the task_struct slab cache:
    task_struct_cachep = kmem_cache_create_usercopy("task_struct",
            arch_task_struct_size, align,
            SLAB_PANIC | SLAB_ACCOUNT,
            useroffset, usersize, NULL);
#endif
    
    // Set maximum threads based on memory:
    // Rule: 1/8 of total RAM, but at least MIN_THREADS
    set_max_threads(THIS_MAXTHREADS);
    
    // Initialize RLIMIT_NPROC for root and users:
    init_task.signal->rlim[RLIMIT_NPROC].rlim_cur = max_threads / 2;
    init_task.signal->rlim[RLIMIT_NPROC].rlim_max = max_threads / 2;
    init_task.signal->rlim[RLIMIT_SIGPENDING] = init_task.signal->rlim[RLIMIT_NPROC];
    
    // Initialize the delayed-execution work for exit:
    for (i = 0; i < UCOUNT_COUNTS; i++) {
        init_user_ns.ucount_max[i] = max_threads / 2;
    }
    
    lockdep_init_task(&init_task);
    uprobes_init();
}
```

## `struct task_struct` Overview

The largest structure in the kernel (~7-8KB). Selected fields:

```c
struct task_struct {
    // State:
    volatile long           state;       // TASK_RUNNING, TASK_INTERRUPTIBLE, etc.
    unsigned int            flags;       // PF_KTHREAD, PF_EXITING, etc.
    
    // Identity:
    pid_t                   pid;         // Thread ID
    pid_t                   tgid;        // Thread group ID (= PID of group leader)
    struct task_struct      *group_leader;
    
    // Scheduling:
    int                     prio;        // Effective priority
    int                     static_prio; // NICE value based priority
    struct sched_entity     se;          // CFS entity
    struct sched_rt_entity  rt;          // RT entity
    struct sched_dl_entity  dl;          // Deadline entity
    
    // Memory:
    struct mm_struct        *mm;         // User address space (NULL for kthreads)
    struct mm_struct        *active_mm;  // Active mm (borrowed by kthreads)
    
    // Credentials:
    const struct cred       *real_cred;  // Objective credentials
    const struct cred       *cred;       // Effective credentials (for exec)
    
    // Signals:
    struct signal_struct    *signal;     // Shared by all threads in group
    struct sighand_struct   *sighand;    // Signal handlers
    sigset_t                blocked;     // Blocked signals
    
    // Files and filesystem:
    struct fs_struct        *fs;         // Working directory, umask
    struct files_struct     *files;      // Open file descriptors
    struct nsproxy          *nsproxy;    // Namespaces (pid, net, mnt, ...)
    
    // Stack:
    void                    *stack;      // Kernel stack pointer
    
    // ... 200+ more fields ...
};
```

## Thread Limit Calculation

```c
// Maximum threads = (total_pages * PAGE_SIZE / THREAD_SIZE) / 8
// On a 4GB system with 8KB stacks:
// = (1,000,000 pages × 4KB / 8KB) / 8 = 62,500 threads

// Minimum: 20 threads (very small systems)
```

## Sub-topic: task_struct Memory Layout

See [task_struct_layout/README.md](task_struct_layout/README.md) for the physical memory layout, including the kernel stack, `thread_info`, and CPU-register save areas.

## Cross-references

- [Phase overview](../README.md)
- `cred_init()`: [../cred_init/README.md](../cred_init/README.md) — credentials in task_struct
- `proc_caches_init()`: [../proc_caches_init/README.md](../proc_caches_init/README.md) — related caches
