# RCU Callback Queuing

## Purpose

Documents how RCU callbacks are queued with `call_rcu()`, segmented by grace period, and eventually invoked after the appropriate grace period completes.

## Source File

`kernel/rcu/tree.c`

## `call_rcu()` Usage

```c
// Example: freeing an RCU-protected data structure
struct my_data {
    struct rcu_head rcu;   // Must be first or accessible
    int value;
};

// Writer:
old_ptr = rcu_dereference(global_ptr);
new_ptr = kmalloc(sizeof(*new_ptr), GFP_KERNEL);
new_ptr->value = new_value;
rcu_assign_pointer(global_ptr, new_ptr);  // Publish new version
call_rcu(&old_ptr->rcu, my_data_free);    // Free old after GP

// Called after next grace period:
void my_data_free(struct rcu_head *head) {
    struct my_data *p = container_of(head, struct my_data, rcu);
    kfree(p);
}
```

## Segmented Callback List

Per-CPU RCU data uses a **segmented callback list** (`struct rcu_segcblist`) with four segments:

```
[RCU_DONE_TAIL]     → Callbacks whose GP is complete (ready to invoke)
[RCU_WAIT_TAIL]     → Waiting for current GP to finish
[RCU_NEXT_READY_TAIL] → GP not yet started for these
[RCU_NEXT_TAIL]     → Newly added callbacks
```

When a grace period completes:
1. `WAIT` segment moves to `DONE`
2. `NEXT_READY` segment moves to `WAIT`
3. `NEXT` segment moves to `NEXT_READY`
4. New `call_rcu()` adds to `NEXT`

## RCU Callback Offloading (nocb)

On NO_HZ_FULL CPUs, invoking callbacks would cause unwanted wakeups. The `rcu_nocbs=` parameter offloads callbacks to a dedicated `rcuo` kthread:

```bash
# Offload RCU callbacks from CPUs 2-7:
rcu_nocbs=2-7
```

The `rcuo/N` kthread runs on a housekeeping CPU and processes callbacks from isolated CPUs.

## `synchronize_rcu()` vs `call_rcu()`

```c
// synchronize_rcu(): BLOCKS caller until GP complete
synchronize_rcu();
// safe to free old data now

// call_rcu(): ASYNC, registers callback for after GP
call_rcu(&head, my_free_fn);
// returns immediately; my_free_fn() called later
```

Rule of thumb: use `call_rcu()` in interrupt/atomic context; `synchronize_rcu()` in process context when you can sleep.

## Cross-references

- [rcu_init parent](../README.md)
- [Grace period kthread](../gp_kthread/README.md)
