# `softirq_init()` — Software Interrupt System

## Purpose

Initializes the software interrupt (softirq) mechanism — a deferred processing system that runs in interrupt context but with interrupts re-enabled, used for high-volume interrupt post-processing like networking and block I/O.

## Source File

`kernel/softirq.c`

```c
void __init softirq_init(void)
{
    int cpu;
    
    for_each_possible_cpu(cpu) {
        per_cpu(tasklet_vec, cpu).tail = 
            &per_cpu(tasklet_vec, cpu).head;
        per_cpu(tasklet_hi_vec, cpu).tail = 
            &per_cpu(tasklet_hi_vec, cpu).head;
    }
    
    open_softirq(TASKLET_SOFTIRQ, tasklet_action);
    open_softirq(HI_SOFTIRQ, tasklet_hi_action);
}
```

## The Softirq Problem

Hardware interrupt handlers must be extremely fast — they run with interrupts disabled on the CPU. But some work is too slow to do in the hard IRQ handler (e.g., processing a batch of network packets).

The solution: **defer** the work to a softirq, which runs shortly after the IRQ handler returns, with interrupts re-enabled.

## The 10 Softirq Vectors

```c
enum {
    HI_SOFTIRQ=0,       // Highest priority — high-priority tasklets
    TIMER_SOFTIRQ,      // Timer wheel expiry
    NET_TX_SOFTIRQ,     // Network transmit
    NET_RX_SOFTIRQ,     // Network receive (most active)
    BLOCK_SOFTIRQ,      // Block device I/O completion
    IRQ_POLL_SOFTIRQ,   // IRQ polling (NAPI-like for IRQs)
    TASKLET_SOFTIRQ,    // Regular tasklets
    SCHED_SOFTIRQ,      // Scheduler (load balancing)
    HRTIMER_SOFTIRQ,    // High-resolution timer expiry
    RCU_SOFTIRQ,        // RCU callback processing
    NR_SOFTIRQS
};
```

## Execution Context

Softirqs run in:
1. **Exit from hardware IRQ handler**: `irq_exit()` → `__do_softirq()`
2. **`ksoftirqd` kernel thread**: when softirq load is too high (avoids starvation of other threads)
3. **Explicit `local_bh_enable()`**: when BH (bottom-half) processing is re-enabled

## `ksoftirqd` Threads

One `ksoftirqd/N` thread per CPU, runs at `SCHED_NORMAL` priority -20 (equivalent to nice 0):

```bash
$ ps aux | grep ksoftirqd
root        12  0.0  0.0      0     0 ?  S    00:00   0:00 [ksoftirqd/0]
root        21  0.0  0.0      0     0 ?  S    00:00   0:00 [ksoftirqd/1]
```

When `__do_softirq()` loops too many times (> `MAX_SOFTIRQ_RESTART = 10`), it wakes `ksoftirqd` to avoid monopolizing the CPU.

## Tasklets: One-Off Softirq Work

Tasklets are a convenient way to schedule one-off deferred work using `TASKLET_SOFTIRQ`:

```c
// Declare a tasklet:
void my_handler(unsigned long data) { /* ... */ }
DECLARE_TASKLET(my_tasklet, my_handler, 0UL);

// Schedule from IRQ handler:
tasklet_schedule(&my_tasklet);
```

Tasklets guarantee:
- Handler runs only once even if scheduled multiple times
- Runs on the CPU that scheduled it (unless using `tasklet_hi_schedule`)
- Not concurrent with itself (serialized per tasklet)

## Pre-conditions

- Per-CPU areas set up
- `ksoftirqd` threads will be created later (after fork infrastructure)

## Post-conditions

- `open_softirq()` has registered `HI_SOFTIRQ` and `TASKLET_SOFTIRQ`
- Per-CPU tasklet vectors initialized
- `raise_softirq()` is safe to call

## Cross-references

- [Phase overview](../README.md)
- `init_timers()`: [../../10_timekeeping_timers/init_timers/README.md](../../10_timekeeping_timers/init_timers/README.md) — registers `TIMER_SOFTIRQ`
