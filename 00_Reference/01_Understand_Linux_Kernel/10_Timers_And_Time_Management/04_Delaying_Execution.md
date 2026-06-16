# 04 — Delaying Execution

## 1. Overview

Kernel code sometimes needs to wait for hardware to settle or for a time period to pass. The method depends on **how long** and **what context**:

```mermaid
flowchart TD
    A[Need to delay] --> B{In atomic context?\n(IRQ, spinlock)}
    B -- Yes --> C{Duration?}
    B -- No --> D{Duration?}
    
    C -->|"< 10 µs"| E["ndelay(n)\nudelay(n < 10)"]
    C -->|"10µs–1ms"| F["udelay(n)"]
    C -->|"> 1ms"| G["mdelay(n) — BAD!\nRefactor to avoid"]
    
    D -->|"< 1 tick"| H["udelay() or\nndelay()"]
    D -->|"1–many ticks"| I["schedule_timeout()\nschedule_timeout_interruptible()"]
    D -->|"1ms–minutes"| J["msleep(ms)\nssleep(secs)\nmsleep_interruptible(ms)"]
```

---

## 2. Busy-Wait Delays (Atomic-Safe)

```c
#include <linux/delay.h>

/* Nanosecond busy-wait — uses TSC, very short */
ndelay(200);       /* Busy-wait ~200 nanoseconds */

/* Microsecond busy-wait — suitable for up to ~10ms */
udelay(10);        /* Busy-wait ~10 microseconds */

/* Millisecond busy-wait — AVOID for > 1-2ms */
mdelay(5);         /* Busy-wait 5ms — wastes CPU for entire duration */
```

> **Warning:** `mdelay()` busy-waits and wastes CPU. For > 1ms in process context, use `msleep()`.

---

## 3. Sleeping Delays (Process Context Only)

```c
/* Sleep for at least N milliseconds (can be longer due to scheduling) */
msleep(100);                    /* Uninterruptible, 100ms minimum */
msleep_interruptible(100);      /* Returns early if signal received */
ssleep(5);                      /* Sleep 5 seconds */

/* Examples */
void wait_for_hardware(void)
{
    /* Pause after hardware reset */
    msleep(50);   /* 50ms — hardware needs time to settle */
}
```

---

## 4. schedule_timeout() — Exact Timeout Control

```c
/* Set current task state and sleep until timeout OR wakeup */
set_current_state(TASK_INTERRUPTIBLE);
schedule_timeout(msecs_to_jiffies(500));  /* Sleep up to 500ms */

/* Convenience wrappers: */
schedule_timeout_interruptible(HZ);    /* Sleep up to 1s, woken by signal */
schedule_timeout_uninterruptible(HZ);  /* Sleep up to 1s, signal won't wake */
schedule_timeout_killable(HZ);         /* Sleep; SIGKILL/SIGTERM will wake */

/* Return value: remaining jiffies (>0 if woken early) */
long remaining = schedule_timeout_interruptible(5 * HZ);
if (remaining > 0)
    /* woken by signal before 5s elapsed */;
```

---

## 5. wait_event Variants (Event + Timeout)

```c
#include <linux/wait.h>

wait_queue_head_t my_wq;
init_waitqueue_head(&my_wq);

/* In waiter: sleep until condition is true */
wait_event_interruptible(my_wq, condition);
wait_event_timeout(my_wq, condition, timeout);             /* Returns remaining */
wait_event_interruptible_timeout(my_wq, condition, timeout);

/* In waker: wake sleeping tasks */
wake_up(&my_wq);
wake_up_all(&my_wq);
wake_up_interruptible(&my_wq);
```

---

## 6. Delay Comparison Table

| Function | Context | Sleep? | Accuracy | When to Use |
|----------|---------|--------|----------|-------------|
| `ndelay(ns)` | Any | No | ~ns | Very short, atomic |
| `udelay(us)` | Any | No | ~us | Short, atomic context |
| `mdelay(ms)` | Any | No | ~ms | Avoid for > 1ms |
| `msleep(ms)` | Process | Yes | ~1ms–10ms | Most common for drivers |
| `ssleep(s)` | Process | Yes | ~1s | Long waits |
| `schedule_timeout()` | Process | Yes | 1 jiffy | With wakeup support |
| `usleep_range(min,max)` | Process | Yes | ~us | Efficient short sleeps |

```c
/* usleep_range — preferred for microsecond sleeps in process context */
usleep_range(1000, 1500);   /* Sleep 1000-1500 microseconds */
```

---

## 7. Source Files

| File | Description |
|------|-------------|
| `include/linux/delay.h` | udelay, mdelay, msleep |
| `kernel/time/timer.c` | schedule_timeout |
| `include/linux/wait.h` | wait_event, wait_queue |

---

## 8. Related Concepts
- [02_Kernel_Timers.md](./02_Kernel_Timers.md) — Asynchronous timer callbacks
- [../09_Kernel_Synchronization_Methods/05_Mutex.md](../09_Kernel_Synchronization_Methods/05_Mutex.md) — Sleeping locks
- [../07_Bottom_Halves_And_Deferring_Work/04_Work_Queues.md](../07_Bottom_Halves_And_Deferring_Work/04_Work_Queues.md) — Process context for sleepable work
