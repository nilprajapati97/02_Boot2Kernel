# Phase 10: Timekeeping and Timers

## Overview

This phase establishes all kernel time-related subsystems: the timer wheel (for `mod_timer()`), high-resolution timers (`hrtimer`), SRCU (a sleepable RCU variant), and the actual wall clock / monotonic clock. After this phase, the kernel knows what time it is and can schedule future work precisely.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`init_timers()`](init_timers/README.md) | `kernel/time/timer.c` | Timer wheel initialization |
| 2 | [`srcu_init()`](srcu_init/README.md) | `kernel/rcu/srcutree.c` | Sleepable RCU initialization |
| 3 | [`hrtimers_init()`](hrtimers_init/README.md) | `kernel/time/hrtimer.c` | High-resolution timers |
| 4 | [`timekeeping_init()`](timekeeping_init/README.md) | `kernel/time/timekeeping.c` | Wall clock, monotonic clock |
| 5 | [`time_init()`](time_init/README.md) | `arch/x86/kernel/time.c` | Architecture time source setup |

## IRQ State

- **Entry**: Enabled (IRQs enabled at end of Phase 9)
- **Exit**: Enabled

## Time Hierarchy in Linux

```
Hardware Clocksources:
  TSC (Time Stamp Counter)   ← x86 preferred (RDTSC)
  HPET (High Precision Event Timer)
  ACPI PM timer
  PIT (8254 Programmable Interval Timer) ← legacy fallback

                ↓ selected by clocksource subsystem

Clocksource (software):
  struct timekeeper            ← master time state
    .clock → selected clocksource
    .xtime_sec, xtime_nsec    ← wall clock (CLOCK_REALTIME)
    .monotonic_time            ← CLOCK_MONOTONIC

                ↓

User APIs:
  clock_gettime(CLOCK_REALTIME)   → wall time
  clock_gettime(CLOCK_MONOTONIC)  → monotonic (no leap seconds)
  gettimeofday()                   → wall time (legacy)
```

## Function Index

- [init_timers/](init_timers/README.md)
- [srcu_init/](srcu_init/README.md)
- [hrtimers_init/](hrtimers_init/README.md)
- [timekeeping_init/](timekeeping_init/README.md)
- [time_init/](time_init/README.md)
