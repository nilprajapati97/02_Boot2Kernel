# `init_timers()` — Timer Wheel Initialization

## Purpose

Initializes the per-CPU timer wheel — the data structure behind `mod_timer()`, `add_timer()`, `del_timer()`, and `schedule_timeout()`. The timer wheel is optimized for managing thousands of concurrent timers with O(1) add/remove in the common case.

## Source File

`kernel/time/timer.c`

```c
void __init init_timers(void)
{
    init_timer_cpus();
    posix_cputimers_init_work();
    open_softirq(TIMER_SOFTIRQ, run_timer_softirq);
}
```

## The Timer Wheel Design

The timer wheel uses multiple levels (buckets) indexed by the time delta until expiry:

```
Level 0: Granularity = 1 jiffie (4ms at HZ=250)
  Slots 0..63: Timers expiring within 64 jiffies (~256ms)

Level 1: Granularity = 64 jiffies (~256ms)  
  Slots 0..63: Timers expiring within 64*64 = 4096 jiffies (~16s)

Level 2: Granularity = 4096 jiffies (~16s)
  Slots 0..63: Timers expiring within ~1024s

Level 3: Granularity = ~17m
Level 4: Granularity = ~18h
```

Adding a timer: O(1) — compute bucket, add to list
Expiring timers: O(n) for n timers in the slot, but typically O(1) amortized

## Timer Operations

```c
// Create and start a timer:
struct timer_list my_timer;
timer_setup(&my_timer, my_callback, 0);
mod_timer(&my_timer, jiffies + msecs_to_jiffies(1000));  // 1 second

// Callback:
void my_callback(struct timer_list *t) {
    // Called from TIMER_SOFTIRQ context
    // Can reschedule: mod_timer(t, jiffies + HZ);
}

// Cancel:
del_timer_sync(&my_timer);  // Waits for any running callback to finish
```

## `TIMER_SOFTIRQ`

Timer expiry runs in softirq context (`TIMER_SOFTIRQ`):

```
Tick interrupt → scheduler_tick() 
    → timer_tick() 
        → raise_softirq(TIMER_SOFTIRQ)
            → [softirq] run_timer_softirq()
                → __run_timers()
                    → calls callbacks for expired timers
```

## jiffies

`jiffies` is the coarse-grained time counter, incremented each tick:

```c
extern unsigned long volatile jiffies;  // Wraps ~49 days at HZ=1000
extern u64 jiffies_64;                  // Never wraps (64-bit)
```

Common time computations:
```c
msecs_to_jiffies(1000)  → HZ     (1 second worth of ticks)
jiffies_to_msecs(HZ)    → 1000   (ms per second)
time_after(a, b)        → true if jiffies 'a' is after 'b' (handles wrap)
```

## Pre-conditions

- `softirq_init()` complete (to register TIMER_SOFTIRQ)
- IRQs enabled (or at least about to be)

## Post-conditions

- `TIMER_SOFTIRQ` registered
- Per-CPU timer wheels allocated and initialized
- `mod_timer()`, `add_timer()`, `del_timer()` functional

## Cross-references

- [Phase overview](../README.md)
- `hrtimers_init()`: [../hrtimers_init/README.md](../hrtimers_init/README.md) — high-resolution timers
