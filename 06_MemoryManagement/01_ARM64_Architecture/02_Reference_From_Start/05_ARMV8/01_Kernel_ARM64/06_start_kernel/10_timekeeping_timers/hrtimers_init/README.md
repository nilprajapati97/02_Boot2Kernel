# `hrtimers_init()` — High-Resolution Timers

## Purpose

Initializes the hrtimer (high-resolution timer) subsystem, which provides nanosecond-granularity timer events. Used for `nanosleep()`, `clock_nanosleep()`, `itimer`, and the POSIX timer infrastructure.

## Source File

`kernel/time/hrtimer.c`

```c
void __init hrtimers_init(void)
{
    hrtimers_prepare_cpu(smp_processor_id());
    open_softirq(HRTIMER_SOFTIRQ, hrtimer_run_softirq);
}
```

## hrtimer vs Timer Wheel

| Feature | Timer Wheel | hrtimer |
|---------|-------------|---------|
| Granularity | 1 jiffie (4ms at HZ=250) | 1 nanosecond |
| Add/Remove | O(1) | O(log n) |
| Overhead | Lower | Higher |
| Use case | Coarse timeouts (TCP retransmit, etc.) | Precise sleep, POSIX timers |

## hrtimer Internal Structure

```c
struct hrtimer {
    struct timerqueue_node  node;        // RB tree node
    ktime_t                 _softexpires;// Expiry time (nanoseconds)
    enum hrtimer_restart    (*function)(struct hrtimer *); // Callback
    struct hrtimer_clock_base *base;    // Which clock this uses
    u8                      state;       // INACTIVE / ENQUEUED / ...
    /* ... */
};
```

Timers are stored in a per-CPU, per-clock red-black tree ordered by expiry time. The leftmost node (earliest expiry) is always cached.

## Clock Bases

Each CPU has multiple clock bases:

```c
enum hrtimer_base_type {
    HRTIMER_BASE_MONOTONIC,     // CLOCK_MONOTONIC
    HRTIMER_BASE_REALTIME,      // CLOCK_REALTIME
    HRTIMER_BASE_BOOTTIME,      // CLOCK_BOOTTIME (includes suspend)
    HRTIMER_BASE_TAI,           // CLOCK_TAI (International Atomic Time)
    HRTIMER_BASE_MONOTONIC_SOFT,// Soft versions (tick-based fallback)
    HRTIMER_BASE_REALTIME_SOFT,
    HRTIMER_BASE_BOOTTIME_SOFT,
    HRTIMER_BASE_TAI_SOFT,
    HRTIMER_MAX_CLOCK_BASES,
};
```

## How hrtimers Fire

### With Hardware High-Resolution Timer Support

When `CONFIG_HIGH_RES_TIMERS` and hardware supports one-shot mode:
- `hrtimer_start()` programs the hardware to interrupt at the exact expiry time
- Interrupt fires → `hrtimer_interrupt()` → calls expired timers

### Without High-Resolution Support

Falls back to HRTIMER_SOFTIRQ triggered by the regular tick:
- `run_hrtimer_softirq()` called each tick
- Processes expired timers (up to 1 tick granularity)

## Usage Example

```c
// Sleep for exactly 1 millisecond:
struct hrtimer_sleeper t;
hrtimer_init_sleeper_on_stack(&t, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
hrtimer_set_expires(&t.timer, ns_to_ktime(1000000)); // 1ms in ns
hrtimer_start_expires(&t.timer, HRTIMER_MODE_REL);
schedule();  // Woken by hrtimer callback
hrtimer_destroy_sleeper_on_stack(&t);
```

## `nanosleep()` Implementation

`nanosleep()` and `clock_nanosleep()` use hrtimers internally:

```
nanosleep(1000000)  // Sleep 1ms
    → hrtimer_start() with 1ms expiry on CLOCK_MONOTONIC
    → task blocked (state = TASK_INTERRUPTIBLE)
    → hrtimer fires
        → sets task to TASK_RUNNING
        → wake_up_process()
    → task resumes
```

## Pre-conditions

- Clocksource available (needed by `timekeeping_init()`)
- `softirq_init()` complete

## Post-conditions

- `hrtimer_start()`, `hrtimer_cancel()` functional
- `nanosleep()` will work (after `timekeeping_init()`)
- `HRTIMER_SOFTIRQ` registered

## Cross-references

- [Phase overview](../README.md)
- `timekeeping_init()`: [../timekeeping_init/README.md](../timekeeping_init/README.md) — provides clock source
