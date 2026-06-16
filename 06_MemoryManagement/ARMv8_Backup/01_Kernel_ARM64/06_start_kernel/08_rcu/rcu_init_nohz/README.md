# `rcu_init_nohz()` — RCU + NO_HZ Integration

## Purpose

Configures RCU to work correctly with tickless (NO_HZ_FULL) CPUs. When CPUs stop their scheduler tick, RCU needs alternative mechanisms to detect quiescent states.

## Source File

`kernel/rcu/tree_plugin.h`

## The Problem: Ticks Needed for Quiescent States

Normally, RCU detects quiescent states during the scheduler tick:

```
Scheduler tick fires on CPU N every 1ms:
    → scheduler_tick()
        → rcu_sched_clock_irq()
            → rcu_check_quiescent_state()
                → if not in RCU read-side: report QS!
```

When NO_HZ_FULL stops the tick on a CPU, RCU never gets this chance.

## The Solution: Context Tracking + RCU Callbacks

With context tracking enabled:
- CPU entering user space → `rcu_user_enter()` → immediate QS report
- CPU returning to kernel → `rcu_user_exit()`

For CPUs that are truly idle (no user or kernel work):
- CPU goes idle → `rcu_idle_enter()` → immediate QS report

## `rcu_init_nohz()` Operations

```c
void __init rcu_init_nohz(void)
{
    // Check if nohz_full CPUs are configured
    if (!tick_nohz_full_enabled())
        return;
    
    // For each nohz_full CPU, ensure it's also in rcu_nocbs
    // (offload RCU callbacks to housekeeping CPUs)
    cpumask_or(rcu_nocb_mask, rcu_nocb_mask, tick_nohz_full_mask);
    
    // Set up per-CPU RCU-nocb kthreads
    rcu_spawn_cpu_nocb_kthread(cpu);
}
```

## The `rcu_nocb` kthreads

For each NO_HZ_FULL CPU, a corresponding `rcuop/N` kthread is created on a housekeeping CPU:

```
CPU 0 (housekeeping):  runs rcuop/2 for CPU 2
CPU 0 (housekeeping):  runs rcuop/3 for CPU 3
...

CPU 2 (isolated):  no RCU callbacks invoked here
CPU 3 (isolated):  no RCU callbacks invoked here
```

## Quiescent State Sources Without Ticks

| Mechanism | Trigger |
|-----------|---------|
| User space entry | `rcu_user_enter()` via context_tracking |
| CPU idle | `rcu_idle_enter()` |
| `cond_resched()` | Explicit yield points in long kernel loops |
| IPI from GP kthread | Forced QS collection for stuck CPUs |

## Kconfig

- `CONFIG_NO_HZ_FULL`: Full dynticks (requires `CONFIG_CONTEXT_TRACKING`)
- `CONFIG_RCU_NOCB_CPU`: Enable callback offloading

## Cross-references

- [Phase overview](../README.md)
- `rcu_init()`: [../rcu_init/README.md](../rcu_init/README.md)
- `context_tracking_init()`: [../../05_tracing_debugging/context_tracking_init/README.md](../../05_tracing_debugging/context_tracking_init/README.md)
