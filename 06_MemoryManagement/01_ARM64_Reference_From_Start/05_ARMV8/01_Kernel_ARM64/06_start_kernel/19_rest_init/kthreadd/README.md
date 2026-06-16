# `kthreadd` — Kernel Thread Daemon (PID 2)

## Purpose

`kthreadd` is the kernel thread daemon, always running as PID 2. It is the parent of all kernel threads created with `kthread_create()`. Its sole job is to process requests to create new kernel threads.

## Source File

`kernel/kthread.c`

```c
int kthreadd(void *unused)
{
    struct task_struct *tsk = current;
    
    /* Setup a clean context for our children to inherit. */
    set_task_comm(tsk, "kthreadd");
    ignore_signals(tsk);
    set_cpus_allowed_ptr(tsk, housekeeping_cpumask(HK_TYPE_KTHREAD));
    set_mems_allowed(node_states[N_MEMORY]);
    
    current->flags |= PF_NOFREEZE;
    cgroup_init_kthreadd();
    
    for (;;) {
        set_current_state(TASK_INTERRUPTIBLE);
        
        if (list_empty(&kthread_create_list))
            schedule();  // Sleep if no work
        
        __set_current_state(TASK_RUNNING);
        
        spin_lock(&kthread_create_lock);
        while (!list_empty(&kthread_create_list)) {
            struct kthread_create_info *create;
            
            create = list_entry(kthread_create_list.next,
                                struct kthread_create_info, list);
            list_del_init(&create->list);
            spin_unlock(&kthread_create_lock);
            
            create_kthread(create);  // Fork and start the thread
            
            spin_lock(&kthread_create_lock);
        }
        spin_unlock(&kthread_create_lock);
    }
    
    return 0;
}
```

## `kthread_create()` Mechanism

When driver code calls `kthread_create()`:

```c
struct task_struct *kthread_create(int (*threadfn)(void *data),
                                    void *data, const char namefmt[], ...)
{
    // 1. Allocate kthread_create_info:
    struct kthread_create_info *create = kmalloc(sizeof(*create), GFP_KERNEL);
    create->threadfn = threadfn;
    create->data = data;
    
    // 2. Add to kthreadd's work list:
    spin_lock(&kthread_create_lock);
    list_add_tail(&create->list, &kthread_create_list);
    spin_unlock(&kthread_create_lock);
    
    // 3. Wake kthreadd:
    wake_up_process(kthreadd_task);
    
    // 4. Wait for kthreadd to create the thread:
    wait_for_completion(&create->done);
    
    return create->result;  // The new task_struct
}
```

## Kernel Threads Created by kthreadd

All kernel threads visible in `ps aux` with brackets are kthreadd children:

```bash
ps aux | grep '\[' | head -20
# PID   PPID  COMMAND
# 2     0     [kthreadd]
# 3     2     [rcu_gp]
# 4     2     [rcu_par_gp]
# 9     2     [mm_percpu_wq]
# 10    2     [rcu_tasks_rude_]
# 11    2     [rcu_tasks_trace]
# 12    2     [ksoftirqd/0]
# 14    2     [kworker/0:1H]
# 15    2     [kdevtmpfs]
# 20    2     [kblockd]
# 23    2     [kswapd0]
# 30    2     [kthrotld]
# 47    2     [kworker/u4:1]
# 89    2     [jbd2/sda1-8]     ← ext4 journal thread
# 90    2     [ext4-rsv-conver] ← ext4 reservation convert
```

## Important Kernel Thread Types

| Name Pattern | Purpose |
|-------------|---------|
| `kworker/N:M` | Workqueue worker for CPU N |
| `ksoftirqd/N` | Softirq processing for CPU N |
| `kswapd0` | Page reclaim/swapping |
| `kthrotld` | Block I/O throttle daemon |
| `rcu_gp` | RCU grace period processing |
| `rcuop/N` | RCU nocb offload for CPU N |
| `jbd2/X` | ext4 journal commit thread |
| `migration/N` | CFS load balancer for CPU N |
| `watchdog/N` | Lockup watchdog for CPU N |

## `kthread_run()` Shorthand

```c
// Combines kthread_create() and wake_up_process():
#define kthread_run(threadfn, data, namefmt, ...)    \
({                                                    \
    struct task_struct *__k;                          \
    __k = kthread_create(threadfn, data, namefmt, ##__VA_ARGS__); \
    if (!IS_ERR(__k))                                 \
        wake_up_process(__k);                         \
    __k;                                              \
})
```

## kthread vs User Thread

| | Kernel Thread | User Thread |
|--|--------------|-------------|
| mm | NULL (borrows active_mm) | Own mm_struct |
| Address space | Kernel only | User + kernel |
| Created by | `kernel_thread()` / `kthread_create()` | `clone()` / `pthread_create()` |
| Can sleep | Yes (TASK_INTERRUPTIBLE) | Yes |
| Signals | Ignored (mostly) | Full signal support |
| Exit | `do_exit()` | `do_exit()` |

## Cross-references

- [Phase overview](../README.md)
- `workqueue_init_early()`: [../../06_scheduling/workqueue_init_early/README.md](../../06_scheduling/workqueue_init_early/README.md)
- `rest_init()`: [../rest_init/README.md](../rest_init/README.md)
