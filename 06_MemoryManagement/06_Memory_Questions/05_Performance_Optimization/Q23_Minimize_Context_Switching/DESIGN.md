# Q23 — Design a System to Minimize Context Switching Overhead

---

## 1. Problem Statement

Context switching is expensive: saving/restoring CPU state (registers, FPU/SIMD state, TLB), cache displacement, and pipeline flush. At 100K context switches/sec, the cost becomes measurable. For GPU driver workloads, userspace ML inference pipelines, and high-frequency trading systems, unnecessary context switches add latency tail.

Design a system that minimizes context switching overhead through preemption control, CPU pinning, cooperative scheduling, and hardware state management.

---

## 2. Requirements

### 2.1 Functional Requirements
- Reduce involuntary context switches (preemptions by scheduler).
- Reduce voluntary context switches (blocking on locks, I/O, sleep).
- Preserve CPU-local state (caches, TLB) across short waits.
- Allow user-space yield control (busy-wait + bounded sleep).
- Measure and monitor context switch rates per-thread.

### 2.2 Non-Functional Requirements
- Context switch rate: < 1000/sec for pinned compute threads.
- Context switch cost: measure with perf and target < 5 µs overhead per switch.
- No starvation: other processes must still make progress.

---

## 3. Constraints & Assumptions

- Linux scheduler (CFS + SCHED_FIFO).
- Multi-core system, each compute thread bound to a dedicated CPU.
- `CONFIG_HZ=1000` (1 ms timer tick).
- Workload: sustained compute (ML inference) with occasional short blocking I/O.

---

## 4. Architecture Overview

```
  ┌──────────────────────────────────────────────────────────────┐
  │  Sources of Context Switches                                 │
  │                                                              │
  │  Involuntary (scheduler-initiated):                         │
  │  ├── Scheduler tick (every 1/HZ ms): may preempt if a      │
  │  │   higher-vruntime task exists                            │
  │  ├── IRQ handler wakes up higher-priority task              │
  │  └── sched_yield() from another task                        │
  │                                                              │
  │  Voluntary (task-initiated):                                │
  │  ├── Block on mutex/semaphore                               │
  │  ├── sleep(), nanosleep(), futex_wait()                     │
  │  ├── Page fault (I/O wait)                                  │
  │  └── System call that blocks (read() on slow fd)            │
  │                                                              │
  │  Mitigation Strategy:                                       │
  │  ┌─────────────────────────────────────────────────────┐   │
  │  │ 1. CPU isolation (isolcpus) → no competing tasks    │   │
  │  │ 2. SCHED_FIFO → no involuntary preemption by CFS    │   │
  │  │ 3. Lock-free data paths → no mutex waits            │   │
  │  │ 4. Busy-poll I/O (io_uring) → no blocking syscalls  │   │
  │  │ 5. NOHZ_FULL → no scheduler tick interruption        │   │
  │  │ 6. mlockall → no page fault interruptions            │   │
  │  └─────────────────────────────────────────────────────┘   │
  └──────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Task Context Switch Counters

```c
struct task_struct {
    /* ... */
    unsigned long  nvcsw;    /* number of voluntary context switches */
    unsigned long  nivcsw;   /* number of involuntary context switches */
    /* Monitor per-thread: */
    /* /proc/<pid>/status: voluntary_ctxt_switches, nonvoluntary_ctxt_switches */
};
```

### 5.2 CPU Context Save Region

```c
/* Full x86-64 context saved by __switch_to() */
struct thread_struct {
    struct desc_struct  tls_array[GDT_ENTRY_TLS_ENTRIES]; /* TLS segments */
    unsigned long       sp;       /* kernel stack pointer */
    unsigned short      es, ds, fsindex, gsindex;
    unsigned long       fsbase, gsbase;   /* FS/GS base for TLS */
    struct fpu          fpu;              /* FPU/SIMD state (xsave area) */
};

/* FPU/SIMD state (lazy save): */
struct fpu {
    unsigned int  last_cpu;    /* which CPU last used this FPU state */
    bool          initialized;
    union fpregs_state  state; /* xsave area: 2.5KB for AVX-512 state */
};
/* Lazy FPU: not saved/restored on every switch — only when FPU is actually used */
```

### 5.3 Runqueue and vruntime

```c
struct cfs_rq {
    struct load_weight  load;
    unsigned int        nr_running;    /* tasks on this CFS runqueue */
    u64                 min_vruntime;  /* minimum vruntime of runnable tasks */
    struct rb_root_cached tasks_timeline; /* RB-tree sorted by vruntime */
};

struct sched_entity {
    struct load_weight  load;
    struct rb_node      run_node;     /* node in tasks_timeline */
    u64                 vruntime;     /* virtual runtime */
    u64                 exec_start;   /* when this run slice started */
    u64                 sum_exec_runtime;
};
/* Context switch: outgoing task's vruntime updated, incoming's exec_start set */
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Context Switch Cost — What Actually Happens

```c
/* context_switch() in kernel/sched/core.c: */
context_switch(rq, prev, next):
    /* 1. Switch memory context */
    if (next->mm != prev->mm):                  /* different address space */
        switch_mm_irqs_off(prev->mm, next->mm)  /* write to CR3 → TLB flush */
        /* Cost: ~100–200 ns for CR3 write + TLB flush */
    else:
        /* kernel threads: no CR3 switch, no TLB flush */

    /* 2. Switch CPU hardware state */
    __switch_to(prev, next):
        /* a. Save prev FS/GS base (MSR writes: ~10 ns each) */
        /* b. Load next FS/GS base (wrmsrl: ~10 ns) */
        /* c. Update TSS.sp0 (kernel stack pointer) */
        /* d. FPU: lazy — NOT saved here unless prev used FPU and next will too */

    /* 3. Stack switch: RSP → next->thread.sp (in __switch_to_asm) */
    /* Total non-TLB cost: ~500 ns–2 µs */

/* TLB flush on CR3 switch: all TLB entries for prev process invalidated */
/* Next access in new process: TLB cold → page table walks → +1–10 µs */
```

### 6.2 Minimizing Involuntary Switches

#### Strategy 1: CPU Isolation + SCHED_FIFO

```bash
# Boot: isolcpus=4-7 — remove CPUs 4-7 from scheduler load balancing
# No other tasks are ever scheduled on CPU 4-7
# Compute thread pinned to CPU 4: never preempted by other tasks

# Set RT priority: kernel won't preempt unless higher RT prio task wakes
sched_setscheduler(tid, SCHED_FIFO, &(struct sched_param){.sched_priority=90});
```

#### Strategy 2: NOHZ_FULL — Eliminate Timer Tick

```bash
# Boot: nohz_full=4-7 rcu_nocbs=4-7
# No HZ tick on CPU 4-7 when only one runnable task
# Effect: RT thread runs uninterrupted for entire period
# Monitor: watch /sys/kernel/debug/sched/domains/cpuN/stats
```

### 6.3 Minimizing Voluntary Switches — Lock-Free Design

```c
/* Replace mutex-protected shared state with per-CPU or lock-free structures */

/* BAD: mutex causes context switch if contended */
mutex_lock(&shared_lock);
result = shared_data[key];
mutex_unlock(&shared_lock);

/* GOOD: per-CPU data — no locking, no context switch */
per_cpu_ptr(my_percpu_data, smp_processor_id())->result = value;

/* GOOD: lock-free ring buffer for producer-consumer */
/* Writer: cmpxchg(tail, old_tail, new_tail) — no blocking */
/* Reader: read head, process, update tail — no blocking */
```

### 6.4 Busy-Polling — Avoid Blocking Syscall

```c
/* Traditional: block on futex → voluntary context switch */
futex_wait(&futex_val, expected, NULL);   /* → context switch */

/* Busy-poll: spin for bounded time before sleeping */
#define SPIN_COUNT 10000
for (int i = 0; i < SPIN_COUNT; i++) {
    if (atomic_read(&futex_val) != expected)
        break;                  /* condition met: no context switch */
    cpu_relax();                /* pause instruction: ~10 ns, hints HT */
}
if (atomic_read(&futex_val) == expected)
    futex_wait(&futex_val, expected, &timeout_1ms);
/* Net effect: if work arrives within 100 µs, zero context switches */
```

### 6.5 FPU/SIMD State Lazy Saving

Context switch cost scales with state size. AVX-512 state = 2.5 KB:
```
Naive: save AVX-512 on every switch = 2.5 KB × 2 (save + restore) per switch
Lazy save (Linux default):
    On context switch: mark FPU as "not active" on next CPU (CR0.TS = 1)
    On first FPU instruction in new context: #NM exception →
        kernel saves prev FPU state NOW (only if prev actually used FPU)
        restores next FPU state
        CR0.TS = 0 (allow FPU again)

For pure integer compute threads: CR0.TS=1 throughout → no FPU save/restore ever
For threads that never use SIMD: zero FPU switching cost
```

### 6.6 KASLR + KPTI — Hidden Context Switch Cost

On CPUs with Meltdown (pre-Cascade Lake): KPTI doubles TLB flush cost:
```
Without KPTI: CR3 switch to new userspace mapping → TLB flush
With KPTI:   CR3 switch to kernel mapping → kernel executes →
             CR3 switch to user mapping → TLB flush (again)
Result: ~2× context switch overhead on affected CPUs

Mitigation: PCID (Process Context IDs) — assigns TLB tag to each address space
    CR3 write with PCID: does NOT flush TLB (hardware maintains per-PCID TLB)
    Result: no TLB flush on context switch if PCID reused
    Linux: uses 12-bit PCID, cycles through 4096 IDs → eviction is rare
```

---

## 7. Trade-off Analysis

| Technique | Context Switches Reduced | CPU Utilization | Latency |
|---|---|---|---|
| isolcpus + SCHED_FIFO | Involuntary: near-zero | Dedicated CPUs wasted if idle | Best |
| NOHZ_FULL | Timer-tick switches: near-zero | Same | Better |
| Busy-poll (bounded) | Short-wait voluntary: reduced | +CPU for polling | Better |
| Lock-free data structures | Lock-contention voluntary: zero | No overhead | Better |
| Kernel threads only | CR3 switch eliminated | Kernel-only capability | N/A |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| context_switch | `kernel/sched/core.c` | `context_switch()`, `__switch_to()` |
| CR3 / TLB | `arch/x86/mm/tlb.c` | `switch_mm_irqs_off()`, `flush_tlb_mm_range()` |
| FPU lazy save | `arch/x86/kernel/fpu/core.c` | `fpu__clear()`, `__fpu_restore_sig()` |
| PCID | `arch/x86/mm/tlb.c` | `choose_new_asid()`, `load_new_mm_cr3()` |
| NOHZ_FULL | `kernel/time/tick-sched.c` | `tick_nohz_full_kick_all()` |
| vruntime update | `kernel/sched/fair.c` | `update_curr()`, `set_next_entity()` |
| Task counters | `kernel/sched/core.c` | `schedule()` → increments nivcsw/nvcsw |

---

## 9. Failure Modes & Debug Strategies

### 9.1 High Involuntary Context Switch Count
```bash
/usr/bin/time -v ./my_program 2>&1 | grep "Involuntary"
# Or: pidstat -w -p <pid> 1   (watch cs rate per second)
# High nivcsw: another task is preempting ours → use SCHED_FIFO or isolcpus
```

### 9.2 High Voluntary Context Switch Count
```bash
pidstat -w -p <pid> 1
# High nvcsw: thread is blocking on locks/I/O
# Identify blockers: strace -c ./program → sys call counts
# Or: perf trace -s -p <pid> 2>&1 | grep "futex\|sleep"
```

### 9.3 TLB Flush Overhead
```bash
perf stat -e dTLB-load-misses,iTLB-load-misses -p <pid> -- sleep 10
# High TLB miss rate + many context switches → PCID not working?
# Check: cat /proc/cpuinfo | grep pcid
```

---

## 10. Performance Considerations

- **Kernel threads:** System calls execute in kernel context — no address space switch (no CR3 change), no TLB flush. Device driver threads have near-zero switch cost.
- **Thread-local storage (TLS):** Access via `%fs`-relative addressing. FS base is restored on each context switch (~10 ns). If TLS access is frequent, keep TLS small to fit in cache.
- **Spinlock vs mutex:** If critical section < 1 µs, spinlock cheaper (no context switch); if > 1 µs, mutex + sleep cheaper (frees CPU for other work).
- **Cache topology:** After a context switch, L1/L2 caches contain prev task's data. For data-intensive workloads, keeping a task on the same CPU (CPU affinity) is critical for warm-cache performance.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Context switch cost breakdown: CR3 write + TLB flush (~200 ns) + register save (~200 ns) + FPU save (0 if lazy) + pipeline flush + cold cache.
2. PCID: modern CPUs tag TLB entries by process, avoiding full TLB flush — know this optimization.
3. Lazy FPU: AVX-512 save only happens when thread actually uses FPU — not every switch.
4. isolcpus + SCHED_FIFO = near-zero involuntary switches.
5. Lock-free + busy-poll = near-zero voluntary switches for short waits.
6. KPTI overhead on pre-Cascade Lake Intel: double CR3 switch per syscall — PCID partially mitigates.
7. pidstat -w: fastest tool to monitor context switch rates per-process.
8. Kernel threads: no MM switch → cheapest context switch type.
