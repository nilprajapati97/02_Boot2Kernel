# 03 — High-Resolution Timers (hrtimer)

## 1. What is hrtimer?

`hrtimer` provides **nanosecond-precision** timers, independent of `jiffies` and the timer tick. They use `ktime_t` (nanosecond values) and the hardware clock directly.

**Used for:** nanosleep, POSIX interval timers, audio latency, real-time tasks.

---

## 2. Data Structure

```c
/* include/linux/hrtimer.h */
struct hrtimer {
    struct timerqueue_node  node;       /* Sorted in rb-tree */
    ktime_t                 _softexpires; /* Expiry time */
    enum hrtimer_restart    (*function)(struct hrtimer *);
    struct hrtimer_clock_base *base;    /* Clock base (MONOTONIC, REALTIME, etc.) */
    u8                      state;      /* HRTIMER_STATE_* */
    /* ... */
};
```

---

## 3. Clock IDs

```c
CLOCK_REALTIME          /* Wall clock (can jump on NTP adjustment) */
CLOCK_MONOTONIC         /* Monotonic since boot (most common) */
CLOCK_BOOTTIME          /* Includes suspend time */
CLOCK_TAI               /* International Atomic Time */
```

---

## 4. API

```c
#include <linux/hrtimer.h>

/* Initialize */
struct hrtimer my_hrtimer;
hrtimer_init(&my_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
my_hrtimer.function = my_hrtimer_fn;

/* Start relative timer (fires 500ms from now) */
hrtimer_start(&my_hrtimer,
              ktime_set(0, 500 * NSEC_PER_MSEC),  /* 500ms in ns */
              HRTIMER_MODE_REL);

/* Start absolute timer */
ktime_t abs_time = ktime_add_ns(ktime_get(), 1000000000LL);  /* now + 1s */
hrtimer_start(&my_hrtimer, abs_time, HRTIMER_MODE_ABS);

/* Cancel */
hrtimer_cancel(&my_hrtimer);     /* Cancels; waits if callback running */
hrtimer_try_to_cancel(&my_hrtimer); /* Non-waiting version */

/* Check status */
hrtimer_active(&my_hrtimer);
hrtimer_is_queued(&my_hrtimer);

/* Forward timer (move expiry forward from callback) */
hrtimer_forward(&my_hrtimer, ktime_get(), ktime_set(0, period_ns));
```

---

## 5. Callback Return Values

```c
static enum hrtimer_restart my_hrtimer_fn(struct hrtimer *timer)
{
    struct my_device *dev = container_of(timer, struct my_device, hrtimer);
    
    /* Do work */
    do_periodic_work(dev);
    
    /* Option 1: Don't reschedule */
    return HRTIMER_NORESTART;
    
    /* Option 2: Reschedule — use hrtimer_forward for precise period */
    hrtimer_forward(timer, ktime_get(), ktime_set(0, dev->period_ns));
    return HRTIMER_RESTART;
}
```

---

## 6. hrtimer vs timer_list

| | timer_list | hrtimer |
|-|-----------|---------|
| Resolution | 1 jiffy (1-10ms) | 1 nanosecond |
| Time source | jiffies | hardware clock (TSC/HPET) |
| Sorted by | Timer wheel slot | RB-tree |
| Use case | Coarse timeouts | Precise timing, nanosleep |
| Context | TIMER_SOFTIRQ | HRTIMER_SOFTIRQ |

---

## 7. High-Resolution Mode

```bash
# Check if hrtimer high-res mode is active:
dmesg | grep "Switched to clocksource"
# "clocksource: Switched to clocksource tsc" → high-res active

cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# tsc (or hpet on older hardware)
```

---

## 8. Source Files

| File | Description |
|------|-------------|
| `include/linux/hrtimer.h` | API |
| `kernel/time/hrtimer.c` | Implementation |
| `kernel/time/posix-timers.c` | POSIX nanosleep, timer_create |

---

## 9. Related Concepts
- [02_Kernel_Timers.md](./02_Kernel_Timers.md) — Low-resolution timers
- [05_Clocksource_And_Clockevents.md](./05_Clocksource_And_Clockevents.md) — Hardware clock infrastructure
- [04_Delaying_Execution.md](./04_Delaying_Execution.md) — Sleeping and delay functions
