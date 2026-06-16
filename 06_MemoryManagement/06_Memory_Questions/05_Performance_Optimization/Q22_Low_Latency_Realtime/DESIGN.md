# Q22 — Design a Low-Latency System for Real-Time Workloads

---

## 1. Problem Statement

Real-time workloads require deterministic, bounded latency — not just average-case performance. A robot control loop running at 1 kHz must complete every iteration within 1 ms, without exception. A GPU driver must service DMA completion interrupts within 100 µs of hardware assertion.

Linux is not a real-time OS by default. Worst-case latency in vanilla Linux can exceed 10 ms due to preemption latency in kernel critical sections, high-resolution timer jitter, interrupt coalescing, and NUMA memory access variance.

Design a low-latency system addressing all kernel-level latency sources, from scheduler configuration to hardware tuning.

---

## 2. Requirements

### 2.1 Functional Requirements
- Real-time task scheduling: SCHED_FIFO / SCHED_DEADLINE with guaranteed CPU time.
- Interrupt latency < 50 µs from IRQ assertion to handler execution.
- Timer latency: hrtimer expiry within ± 10 µs of target.
- Memory access latency: all hot memory locked in RAM (no page faults in RT path).
- CPU isolation: RT tasks not interrupted by non-RT work.

### 2.2 Non-Functional Requirements
- Worst-case latency (WCET) bounded, measurable via `cyclictest`.
- `cyclictest` P99.999 latency: < 100 µs on isolated CPU.
- System still functional for non-RT tasks on remaining CPUs.

---

## 3. Constraints & Assumptions

- PREEMPT_RT fully preemptible kernel (CONFIG_PREEMPT_RT).
- x86-64 or ARM64.
- Dedicated RT CPU cores (isolated via `isolcpus` kernel parameter).
- No virtualization on RT CPUs.

---

## 4. Architecture Overview

```
  System CPUs          Isolated RT CPUs (isolcpus=4-7)
  ┌──────────────┐     ┌──────────────────────────────────┐
  │ CPU 0, 1,2,3 │     │  CPU 4, 5, 6, 7                 │
  │ General OS   │     │  ┌───────────────────────────┐   │
  │  tasks       │     │  │ RT Task (SCHED_FIFO)       │   │
  │  interrupts  │     │  │ mlockall(MCL_CURRENT|      │   │
  │  kernel work │     │  │   MCL_FUTURE)              │   │
  └──────────────┘     │  │ CPU-pinned via sched_setaffinity│
                       │  └───────────────────────────┘   │
                       │  ┌───────────────────────────┐   │
                       │  │ Only critical IRQs allowed │   │
                       │  │ (set irq affinity to these │   │
                       │  │  CPUs for RT device only)  │   │
                       │  └───────────────────────────┘   │
                       └──────────────────────────────────┘

  Latency Sources Eliminated:
  ┌─────────────────────────────────────────────────────────┐
  │ Kernel preemption: PREEMPT_RT converts spinlocks to RT  │
  │ mutexes → spinlock critical sections are now preemptible│
  │                                                         │
  │ IRQ threading: IRQF_THREAD → all IRQs run as RT threads │
  │ → IRQ handler can be preempted by higher-priority RT    │
  │                                                         │
  │ Timer: hrtimer on dedicated clocksource (TSC/HPET)      │
  │ → sub-microsecond resolution                            │
  │                                                         │
  │ Page faults: mlockall() locks all pages in RAM          │
  │ → no demand paging in RT path                           │
  └─────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 RT Task Configuration

```c
struct sched_param {
    int sched_priority;   /* 1–99 for SCHED_FIFO/RR; 0 for SCHED_NORMAL */
};

/* Set real-time scheduling policy */
struct sched_param sp = { .sched_priority = 90 };
sched_setscheduler(0, SCHED_FIFO, &sp);
/*
 * SCHED_FIFO: runs until block, yield, or preempted by higher-priority RT task
 * Priority 90: preempts all normal tasks (prio 0–39); preempted only by prio 91–99
 */

/* SCHED_DEADLINE: EDF with budget control */
struct sched_attr attr = {
    .sched_policy   = SCHED_DEADLINE,
    .sched_runtime  = 500000,    /* 500 µs compute budget per period */
    .sched_deadline = 1000000,   /* 1 ms deadline */
    .sched_period   = 1000000,   /* 1 ms period */
};
sched_setattr(0, &attr, 0);
/* Kernel: uses CBS (Constant Bandwidth Server) to enforce budget */
/* If task overruns runtime: it is throttled until next period */
```

### 5.2 High-Resolution Timer

```c
struct hrtimer {
    struct timerqueue_node  node;      /* RB-tree node (expiry time as key) */
    ktime_t                 _softexpires; /* target expiry */
    enum hrtimer_restart  (*function)(struct hrtimer *); /* callback */
    struct hrtimer_clock_base *base;   /* clock: CLOCK_MONOTONIC, REALTIME */
    unsigned long           state;     /* HRTIMER_STATE_ENQUEUED */
};

/* Setup: */
hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
timer.function = my_rt_callback;
hrtimer_start(&timer, ns_to_ktime(1000000), HRTIMER_MODE_REL); /* 1 ms */
```

### 5.3 CPU Isolation and Affinity

```c
/* Boot parameter: isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 */
/* Effect: CPUs 4-7 removed from scheduler load balancing */
/*         No HZ ticks (NOHZ_FULL) on these CPUs */
/*         No RCU callbacks on these CPUs */

/* Kernel: pin RT task to isolated CPU */
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(4, &cpuset);
sched_setaffinity(pid, sizeof(cpuset), &cpuset);

/* Pin IRQ to RT CPU (driver): */
irq_set_affinity_hint(irq, cpumask_of(4));
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 PREEMPT_RT — Converting Spinlocks to RT Mutexes

In vanilla Linux, `spin_lock()` disables preemption. A high-priority RT task cannot preempt a lower-priority task holding a spinlock. This causes priority inversion and unbounded latency.

PREEMPT_RT converts almost all spinlocks to `rt_mutex`:

```
Vanilla Linux:
    spin_lock():    local_irq_disable() + preempt_disable()
    → RT task blocked for entire critical section duration

PREEMPT_RT:
    spin_lock():    rt_mutex_lock() (sleepable)
    → If RT task blocks on rt_mutex held by low-prio task:
        Priority inheritance: low-prio task gets RT task's priority temporarily
        → Low-prio task preempts medium-prio tasks to finish quickly
        → RT task unblocks sooner

Exceptions (cannot be converted, must stay non-preemptible):
    raw_spin_lock()  — hardware register access, NMI context
    preempt_disable() — RCU critical sections
```

### 6.2 IRQ Threading — All Interrupts as RT Threads

With PREEMPT_RT, interrupt handlers run as kernel threads (not hardirq context):

```
Vanilla: IRQ fires → hardirq context (NMIs, preemption disabled)
PREEMPT_RT: IRQ fires → wakes irq/N-device thread → thread runs at RT priority

Benefits:
    IRQ handler can block (e.g., acquire rt_mutex)
    IRQ handler is preemptible by higher-priority RT threads
    RT task at priority 95 preempts IRQ handler at priority 50

Setup (driver):
    request_threaded_irq(irq, quick_check_handler, threaded_handler,
                         IRQF_ONESHOT, name, dev);
    /* quick_check_handler: minimal work in hardirq context */
    /* threaded_handler: bulk processing in RT thread */
```

### 6.3 Timer Latency — hrtimer + TSC Clocksource

```
hrtimer expiry path (PREEMPT_RT):
    Hardware timer fires (HPET or LAPIC one-shot) →
    hardirq: reads TSC, programs next timer tick →
    wakes hrtimer softirq thread (runs at RT priority) →
    calls hrtimer callback

Latency sources:
    1. TSC read: < 10 ns
    2. Interrupt routing: < 1 µs (LAPIC MSI)
    3. hrtimer thread scheduling: < 10 µs (if CPU is isolated + no contention)
    Total: < 20 µs typical, < 50 µs worst-case with PREEMPT_RT

Clocksource configuration:
    echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource
    # TSC is most precise; HPET is slower but better if TSC is unstable
```

### 6.4 Memory Locking — Eliminating Page Faults

```c
/* Lock all memory before entering RT loop */
ret = mlockall(MCL_CURRENT | MCL_FUTURE);
/*
 * MCL_CURRENT: lock all pages currently mapped
 * MCL_FUTURE: lock all future allocations too
 * Effect: kernel pins all pages in RAM — demand paging disabled for this process
 */

/* Pre-fault stack: force stack pages into RAM before RT loop */
void prefault_stack(void)
{
    volatile char buf[8192];
    /* Touch every page of the stack */
    for (int i = 0; i < sizeof(buf); i += PAGE_SIZE)
        buf[i] = 0;
}

/* Pre-allocate all working memory before RT loop */
/* Never call malloc/free inside the RT loop */
```

### 6.5 NOHZ_FULL — Eliminate Timer Ticks on RT CPUs

Linux's scheduler tick fires every `CONFIG_HZ` ms (250 Hz = 4 ms, 1000 Hz = 1 ms). Each tick interrupts the RT task and runs the scheduler.

With `nohz_full=4-7`:
```
CPUs 4-7: No periodic scheduler tick when exactly one runnable task.
Effect: RT task on CPU 4 runs uninterrupted for its full period.
Remaining tick sources:
    - hrtimer (necessary for SCHED_DEADLINE period tracking)
    - watchdog timer (configurable, can reduce frequency)
    
nohz_full requires: rcu_nocbs=4-7 (RCU callbacks offloaded to CPU 0-3)
```

### 6.6 Cyclictest — Measuring Actual Latency

```bash
# Measure worst-case timer latency on isolated RT CPUs
cyclictest \
    --mlockall \           # lock memory
    --priority=99 \        # SCHED_FIFO priority 99
    --interval=1000 \      # 1 ms period
    --duration=3600 \      # run 1 hour
    --affinity=4 \         # pin to CPU 4
    --histogram=200 \      # collect histogram up to 200 µs
    --threads=4            # 4 RT threads

# Output: Min/Avg/Max latency, histogram CSV
# Target: Max < 100 µs with PREEMPT_RT on isolated CPU
```

---

## 7. Trade-off Analysis

| Configuration | WCET | Overhead | Complexity |
|---|---|---|---|
| Vanilla PREEMPT_DYNAMIC | 10+ ms | None | None |
| PREEMPT_RT | < 200 µs | 5–15% throughput | Significant (all spinlocks become rt_mutex) |
| PREEMPT_RT + isolcpus | < 100 µs | Dedicated CPUs | Yes (CPU resource reservation) |
| PREEMPT_RT + isolcpus + NOHZ | < 50 µs | More dedicated CPUs | High |
| Xenomai/RTAI dual-kernel | < 10 µs | Highest | Highest (separate RT kernel) |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| PREEMPT_RT mutex | `kernel/locking/rtmutex.c` | `rt_mutex_lock()`, `rt_mutex_slowlock()` |
| Priority inheritance | `kernel/locking/rtmutex.c` | `rt_mutex_adjust_prio_chain()` |
| IRQ threading | `kernel/irq/manage.c` | `setup_irq_thread()`, `irq_thread()` |
| hrtimer | `kernel/time/hrtimer.c` | `hrtimer_start()`, `__hrtimer_run_queues()` |
| NOHZ_FULL | `kernel/time/tick-sched.c` | `tick_nohz_full_kick()`, `tick_setup_sched_timer()` |
| SCHED_DEADLINE | `kernel/sched/deadline.c` | `dl_task_timer()`, `enqueue_dl_entity()` |
| CPU isolation | `kernel/sched/isolation.c` | `housekeeping_cpumask()` |
| mlockall | `mm/mlock.c` | `__do_mlockall()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Unexpected Latency Spike (> target)
```bash
# Identify source:
trace-cmd record -e 'irq:*' -e 'sched:sched_switch' -p preemptirqsoff
trace-cmd report | head -200
# Look for: long preempt-off sections, unexpected IRQs on RT CPU
```

### 9.2 SCHED_DEADLINE Task Throttled
```bash
cat /proc/<pid>/sched | grep "nr_throttled"
# Task over-ran its runtime budget → throttled until next period
# Fix: increase runtime or reduce algorithm complexity
```

### 9.3 Memory Allocation in RT Path (stall)
```bash
# Use KASAN or slub_debug to catch unexpected GFP_KERNEL allocations
# In RT thread: check all called functions for kmalloc (audit with sparse)
```

---

## 10. Performance Considerations

- **rt_mutex overhead vs spinlock:** Spinlock is 2–5 ns; rt_mutex is 100–500 ns. Critical sections using rt_mutex should be minimized.
- **Cache warming:** Before RT period begins, touch all data structures to ensure L1/L2 cache is warm. Cold cache miss = 100+ ns stall.
- **IRQ affinity isolation:** Only pin the RT device's IRQ to the RT CPU. All other IRQs must be affinitized away.
- **SMI (System Management Interrupt):** SMI is non-maskable firmware interrupt that can stall CPU for 100 µs–10 ms. Disable in BIOS or use `hwlatdetect` to measure.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. PREEMPT_RT converts spinlocks → rt_mutex with priority inheritance — this is the core mechanism.
2. Priority inversion without PI: low-priority task holds lock needed by high-priority task → medium-priority tasks preempt the low-priority task → deadlock-like unbounded wait.
3. IRQ threading: all device IRQs run as RT threads → preemptible by higher-priority RT tasks.
4. NOHZ_FULL: eliminate scheduler tick on RT CPUs when only one runnable task.
5. mlockall + pre-fault stack: eliminate page faults in RT path.
6. cyclictest: the standard tool for measuring latency — know how to run and interpret it.
7. SMI: the worst enemy of real-time on x86 — hardware-level non-maskable firmware interrupt, not fixable in software alone.
8. SCHED_DEADLINE vs SCHED_FIFO: DEADLINE = EDF with budget enforcement (cannot starve system); FIFO = pure priority (simpler but can starve).
