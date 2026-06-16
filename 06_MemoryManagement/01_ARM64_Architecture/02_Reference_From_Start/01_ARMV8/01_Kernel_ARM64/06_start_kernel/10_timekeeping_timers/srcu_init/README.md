# `srcu_init()` — Sleepable RCU

## Purpose

Initializes SRCU (Sleepable RCU) — an RCU variant where readers are allowed to sleep inside their read-side critical sections. Used by subsystems like filesystem notifications, BPF, and module loading.

## Source File

`kernel/rcu/srcutree.c`

## Why SRCU?

Regular RCU forbids sleeping in `rcu_read_lock()` sections because the grace period mechanism relies on readers making progress (context switches = quiescent states). If a reader sleeps, the GP can never complete.

SRCU uses a different mechanism: **per-SRCU-domain per-CPU counters** that explicitly track active readers.

```c
// Regular RCU (no sleeping allowed):
rcu_read_lock();
ptr = rcu_dereference(global_ptr);
// NO SLEEPING HERE
rcu_read_unlock();

// SRCU (sleeping allowed):
idx = srcu_read_lock(&my_srcu);
ptr = srcu_dereference(global_ptr, &my_srcu);
msleep(100);  // OK! Can sleep
srcu_read_unlock(&my_srcu, idx);
```

## How SRCU Tracks Readers

Each `struct srcu_struct` has per-CPU lock counts:

```c
struct srcu_struct {
    struct srcu_node node[/* tree */];  // Hierarchical like Tree RCU
    struct srcu_data __percpu *sda;     // Per-CPU data
    /* ... */
};

struct srcu_data {
    // Two sets of counters (flip-flop):
    atomic_long_t srcu_lock_count[2];   // Readers acquired
    atomic_long_t srcu_unlock_count[2]; // Readers released
    /* ... */
};
```

`srcu_read_lock()` increments `srcu_lock_count[current_idx]`.
`srcu_read_unlock()` increments `srcu_unlock_count[current_idx]`.

A grace period completes when `lock_count == unlock_count` for the old index on all CPUs.

## SRCU Grace Periods

```c
// Async (via workqueue):
call_srcu(&my_srcu, &head, my_callback);

// Sync (blocks):
synchronize_srcu(&my_srcu);
```

SRCU grace periods are per-domain (each `srcu_struct` is independent), so multiple domains can have simultaneous grace periods.

## `srcu_init()` — Scheduling the SRCU Work

`srcu_init()` is called after `workqueue_init_early()` to schedule deferred SRCU initialization on all CPUs via the workqueue:

```c
void __init srcu_init(void)
{
    // Schedule SRCU per-CPU initialization via workqueue
    srcu_init_done = true;
    while (!list_empty(&srcu_boot_list)) {
        ssp = list_first_entry(&srcu_boot_list, struct srcu_struct, work.entry);
        list_del_init(&ssp->work.entry);
        queue_work(rcu_gp_wq, &ssp->work);
    }
}
```

## Key Users of SRCU

| Subsystem | SRCU domain |
|-----------|-------------|
| inotify/fanotify | `fsnotify_mark_srcu` |
| BPF | `bpf_srcu` |
| Module loading | `module_srcu` |
| NFS | `nfs_callback_lock` |
| SELinux | `selinux_ss_lock` |

## Cross-references

- [Phase overview](../README.md)
- `rcu_init()`: [../../08_rcu/rcu_init/README.md](../../08_rcu/rcu_init/README.md)
