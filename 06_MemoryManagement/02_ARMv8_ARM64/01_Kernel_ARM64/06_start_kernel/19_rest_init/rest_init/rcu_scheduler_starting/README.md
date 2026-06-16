# `rcu_scheduler_starting()` — Enable RCU Preemption

## Purpose

Notifies the RCU subsystem that the scheduler is now fully operational. This transitions RCU from "early boot" mode (no preemption, single CPU) to "normal" mode where it must handle preempted readers and multiple CPUs.

## Source File

`kernel/rcu/tree.c`

```c
void rcu_scheduler_starting(void)
{
    WARN_ON(num_online_cpus() != 1);
    WARN_ON(nr_context_switches() > 0);
    rcu_scheduler_active = RCU_SCHEDULER_RUNNING;
    rcu_test_sync_prims();
}
```

## Before vs After

### Before `rcu_scheduler_starting()`

```
rcu_scheduler_active = RCU_SCHEDULER_INACTIVE

rcu_read_lock()  → does nothing (no scheduler means no preemption)
rcu_read_unlock() → does nothing
synchronize_rcu() → works by checking that no NMI/softirq is running
```

This simplified mode is safe because:
- Only one CPU is running
- No preemption occurs
- No context switches happen

### After `rcu_scheduler_starting()`

```
rcu_scheduler_active = RCU_SCHEDULER_RUNNING

rcu_read_lock()  → increments per-task nesting counter
                   prevents preemption (in PREEMPT_RCU)
rcu_read_unlock() → decrements, reports QS if reached 0
synchronize_rcu() → waits for grace period (CPUs must have quiesced)
```

## Why This Ordering Matters

`rest_init()` calls `rcu_scheduler_starting()` **before** `kernel_thread()`. This ensures that:

1. When `kernel_init` and `kthreadd` are created via `kernel_thread()`, RCU is already in the correct mode
2. The scheduler can preempt these new threads safely
3. RCU grace periods work correctly from the first context switch

## `rcu_test_sync_prims()`

Runs a quick self-test to verify that `synchronize_rcu()` and related primitives work correctly before the rest of the system depends on them.

## Cross-references

- [Parent: rest_init](../README.md)
- `rcu_init()`: [../../08_rcu/rcu_init/README.md](../../08_rcu/rcu_init/README.md) — initial RCU setup
