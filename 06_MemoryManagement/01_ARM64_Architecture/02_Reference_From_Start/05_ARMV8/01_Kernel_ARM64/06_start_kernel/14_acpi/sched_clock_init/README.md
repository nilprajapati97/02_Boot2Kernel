# `sched_clock_init()` — Stable Scheduler Clock

## Purpose

Marks the scheduler clock as stable and initializes the per-CPU `sched_clock_data` structures. After this call, `sched_clock()` returns a reliable nanosecond timestamp suitable for scheduling decisions.

## Source File

`kernel/sched/clock.c`

```c
void __init sched_clock_init(void)
{
    // Mark clock as stable (TSC or HPET fully calibrated by now):
    sched_clock_running = 1;
    
    // Initialize per-CPU clock data:
    local_irq_disable();
    generic_sched_clock_init_early();
    local_irq_enable();
}
```

## Why a Separate Scheduler Clock?

The scheduler needs nanosecond-precision time for:
- CFS vruntime accounting (how much CPU time a task has used)
- Scheduling decision latency (time since last wakeup)
- sched_latency_ns enforcement

The challenge: TSC may not be synchronized across CPUs (especially on multi-socket systems), and it may drift or be unstable on some hardware.

## Clock Source Priority

```
sched_clock() tries in order:
1. TSC (if stable and synchronized)    → ~1 ns, zero overhead
2. HPET                                 → ~100 ns, readable
3. ACPI PM timer                        → ~400 ns
4. jiffies (fallback)                   → ~4ms granularity
```

## The Stability Problem

Before `sched_clock_init()` completes ACPI setup, TSC stability cannot be confirmed. Some platforms have:
- TSC that resets during C-states (unstable)
- Per-socket TSCs that are not synchronized (multi-socket)
- TSC frequency changes with P-states (before `constant_tsc` CPU flag)

## vruntime and Scheduler Clock

```c
// CFS accounts time in nanoseconds:
static void update_curr(struct cfs_rq *cfs_rq)
{
    u64 now = rq_clock_task(rq_of(cfs_rq));  // calls sched_clock()
    u64 delta_exec = now - curr->exec_start;
    curr->vruntime += calc_delta_fair(delta_exec, curr);
}
```

## Cross-references

- [Phase overview](../README.md)
- `timekeeping_init()`: [../../10_timekeeping_timers/timekeeping_init/README.md](../../10_timekeeping_timers/timekeeping_init/README.md)
- `sched_init()`: [../../06_scheduling/sched_init/README.md](../../06_scheduling/sched_init/README.md)
