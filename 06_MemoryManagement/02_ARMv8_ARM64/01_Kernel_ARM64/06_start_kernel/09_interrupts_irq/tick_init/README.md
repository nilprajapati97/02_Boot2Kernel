# `tick_init()` — Timer Tick Framework

## Purpose

Initializes the kernel's tick framework — the infrastructure that drives the scheduler tick, jiffies update, and timer wheel. The tick framework abstracts different hardware timer modes (periodic, one-shot, broadcast) behind a common API.

## Source File

`kernel/time/tick-common.c`

```c
void __init tick_init(void)
{
    tick_broadcast_init();    // Set up tick broadcast for CPU power states
    tick_nohz_init();         // NO_HZ (tickless) support
}
```

## The Tick's Role

Every `HZ` times per second (default 250 on x86 → 4ms tick), a timer interrupt fires and:

1. Updates `jiffies` (coarse-grained time reference)
2. Calls `update_process_times()` → scheduler accounting
3. Calls `run_timer_softirq()` → fire expired timers
4. Calls `calc_global_load()` → load average update
5. Calls `run_posix_cpu_timers()` → POSIX CPU time limits

## HZ Modes

```
CONFIG_HZ_100:   100 ticks/second (10ms) — embedded, low power
CONFIG_HZ_250:   250 ticks/second (4ms)  — default desktop/server
CONFIG_HZ_300:   300 ticks/second (3.3ms)
CONFIG_HZ_1000: 1000 ticks/second (1ms)  — low-latency
```

## NO_HZ: Tickless Operation

The `tick_nohz_init()` call sets up three modes:

### `CONFIG_NO_HZ_IDLE` (default)
When a CPU is idle (no runnable tasks), the periodic tick is stopped:
- Saves power (CPU can enter deeper C-states)
- On wakeup: account for elapsed jiffies at once

### `CONFIG_NO_HZ_FULL`
When a CPU has exactly one runnable task (user-space only), the tick is also stopped:
- Eliminates jitter from scheduler tick for real-time/HPC
- Requires `context_tracking` and `rcu_nocbs`

## Tick Device (`struct tick_device`)

Each CPU has a `struct tick_device` wrapping a `struct clock_event_device`:

```c
struct tick_device {
    struct clock_event_device *evtdev;  // Hardware timer device
    enum tick_device_mode mode;          // PERIODIC or ONESHOT
};
```

### Periodic Mode

The hardware timer fires at fixed HZ intervals. Simple but wastes power.

### One-Shot Mode

The hardware timer fires once at the next "interesting" event (next timer expiry). More efficient for NO_HZ.

## Broadcast Timer

When a CPU enters deep idle states (C3+), its local APIC timer may stop. The broadcast timer fires an IPI to wake CPUs whose timers have expired:

```
CPU 0 (housekeeping): runs broadcast timer
CPU 1 (idle, C3):     timer would have fired at T=100ms
                       → CPU 0 sends IPI to CPU 1 at T=100ms
                       → CPU 1 wakes up, processes tick
```

## Cross-references

- [Phase overview](../README.md)
- `timekeeping_init()`: [../../10_timekeeping_timers/timekeeping_init/README.md](../../10_timekeeping_timers/timekeeping_init/README.md)
- `init_timers()`: [../../10_timekeeping_timers/init_timers/README.md](../../10_timekeeping_timers/init_timers/README.md)
