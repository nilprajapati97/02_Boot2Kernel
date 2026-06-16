# `rcu_init()` — Core RCU Initialization

## Purpose

Initializes the RCU (Read-Copy-Update) subsystem: sets up per-CPU RCU data, the grace period tracking tree, callback queues, and registers the RCU grace period kthread.

## Source File

`kernel/rcu/tree.c`

## RCU Variants in Linux

| Variant | When to Use | Grace Period |
|---------|-------------|--------------|
| `RCU` (`rcu_read_lock`) | Default; preemptible kernel blocks during GP | Regular |
| `RCU-BH` (deprecated) | Was for bottom-half context | Was BH-disabled |
| `RCU-sched` | Non-preemptible readers | Schedule point |
| `SRCU` | Sleepable RCU readers | Per-SRCU-domain |
| `TASKS-RCU` | Userspace task RCU | Schedule/idle |

`rcu_init()` initializes Tree RCU — the main implementation.

## Tree RCU Architecture

RCU uses a **hierarchical tree** of nodes to track quiescent states efficiently on large systems:

```
                    [Root rcu_node]
                   /               \
           [rcu_node]           [rcu_node]
          /         \           /         \
    [rcu_node] [rcu_node] [rcu_node] [rcu_node]
        /\        /\        /\        /\
    CPU CPU   CPU CPU   CPU CPU   CPU CPU
```

Each leaf node covers a set of CPUs. Inner nodes aggregate results. This allows a 1024-CPU system to have only ~O(log N) lock contention for grace period processing.

## Key Data Structures

### `struct rcu_data` (per-CPU)

```c
struct rcu_data {
    // Quiescent state tracking:
    unsigned long   gp_seq;         // GP sequence number observed
    bool            core_needs_qs;  // This CPU needs to report QS
    bool            cpu_no_qs;      // CPU hasn't had QS yet
    
    // Callback management:
    struct rcu_segcblist cblist;    // Segmented callback list
    
    // GP tracking:
    unsigned long   gp_seq_needed;  // GP needed for oldest callback
    
    // Per-CPU RCU kthread (for offloaded callbacks):
    struct task_struct *nocb_gp_kthread;
};
```

### `struct rcu_node` (tree node)

```c
struct rcu_node {
    raw_spinlock_t  lock;
    unsigned long   gp_seq;       // Current GP seq on this node
    unsigned long   gp_seq_needed;// GP needed by any child
    unsigned long   qsmask;       // CPUs that still need to report QS
    unsigned long   qsmaskinit;   // Initial QS mask
    unsigned long   grpmask;      // This node's bit in parent
    int             grplo, grphi; // CPU range covered
    struct rcu_node *parent;
};
```

## Grace Period Lifecycle

```
1. Writer calls synchronize_rcu() or call_rcu(callback, fn)

2. Grace period start:
   - rcu_gp_kthread wakes up
   - Sets gp_seq (new GP number)
   - Broadcasts "new GP" to all CPUs via tree

3. Each CPU reports quiescent state when:
   - Context switch (schedule())
   - CPU goes idle
   - User-space entry
   - explicit rcu_quiescent_state()

4. rcu_node tree propagates QS reports upward

5. Root sees all CPUs reported QS → GP is complete

6. Invoke all callbacks registered during this GP
```

## Sub-topics

- [Grace Period kthread](gp_kthread/README.md)
- [Callback Queuing](callbacks/README.md)

## Pre-conditions

- Per-CPU areas set up
- `kmalloc()` available (for rcu_node allocation)

## Post-conditions

- `rcu_read_lock()` / `rcu_read_unlock()` functional
- `call_rcu()` safe to use
- `synchronize_rcu()` safe to use (blocks until GP)
- RCU kthread registered (starts when `kthread_run` infra ready)

## Cross-references

- [Phase overview](../README.md)
- `rcu_init_nohz()`: [../rcu_init_nohz/README.md](../rcu_init_nohz/README.md)
