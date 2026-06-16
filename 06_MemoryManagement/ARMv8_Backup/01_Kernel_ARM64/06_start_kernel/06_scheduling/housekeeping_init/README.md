# `housekeeping_init()` — CPU Isolation and Housekeeping

## Purpose

Initializes CPU isolation: configures which CPUs are "housekeeping" CPUs (run kernel daemons, timers, RCU callbacks) vs "isolated" CPUs (dedicated to user workloads with minimal kernel interference).

## Source File

`kernel/sched/isolation.c`

## What is CPU Isolation?

In HPC, real-time, and latency-sensitive workloads, kernel activities (RCU callbacks, timer softirqs, scheduler load balancing, per-CPU kthreads) introduce jitter that disrupts application performance.

CPU isolation partitions CPUs into:

```
Housekeeping CPUs:          Isolated CPUs:
  CPU 0, 1                    CPU 2, 3, 4, ..., N-1
  ─────────                   ─────────────────────
  Run kthreads                No kernel threads
  Handle RCU                  No load balancing
  Run timers                  Minimal timer ticks (NO_HZ_FULL)
  Load balance                Only user tasks + syscalls
```

## Command Line Configuration

```bash
# Isolate CPUs 2-7 from kernel housekeeping:
isolcpus=2-7

# Enable tickless operation on CPUs 2-7:
nohz_full=2-7

# Set RCU callbacks to run on CPUs 0-1:
rcu_nocbs=2-7
```

## The `HK_TYPE_*` Flags

```c
enum hk_type {
    HK_TYPE_TIMER,      // Timer interrupts
    HK_TYPE_RCU,        // RCU callbacks
    HK_TYPE_MISC,       // Miscellaneous kernel threads
    HK_TYPE_SCHED,      // Scheduler load balancing
    HK_TYPE_TICK,       // Scheduler tick
    HK_TYPE_DOMAIN,     // Scheduler domains
    HK_TYPE_WQ,         // Workqueue threads
    HK_TYPE_MANAGED_IRQ,// Managed IRQ affinity
    HK_TYPE_KTHREAD,    // Kernel threads
    HK_TYPE_MAX
};
```

Each type has a separate cpumask of housekeeping CPUs.

## Implementation

```c
void __init housekeeping_init(void)
{
    if (!static_branch_unlikely(&housekeeping_overridden))
        return;
    
    // Parse isolcpus= cmdline parameter
    // Set up per-type housekeeping cpumasks
    // Ensure at least one non-isolated CPU exists
    
    for_each_set_bit(type, &housekeeping_flags, HK_TYPE_MAX) {
        cpumask_andnot(housekeeping_mask[type],
                      cpu_possible_mask, isolated_cpumask);
    }
}
```

## Effect on Kernel Behavior

When housekeeping is configured:
- `kthread_create()` → bound to housekeeping CPUs
- `schedule_work()` → runs on housekeeping CPUs  
- RCU callbacks → queued on housekeeping CPUs
- Timer wheel → only ticks on housekeeping CPUs (others use NO_HZ_FULL)

## Pre-conditions

- `sched_init()` complete
- Command line parsed (needs `isolcpus=` values)

## Post-conditions

- Housekeeping cpumasks set
- `housekeeping_any_cpu(HK_TYPE_*)` returns correct CPUs

## Cross-references

- [Phase overview](../README.md)
- `context_tracking_init()`: [../../05_tracing_debugging/context_tracking_init/README.md](../../05_tracing_debugging/context_tracking_init/README.md) — needed for NO_HZ_FULL
