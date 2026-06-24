# Q16 — Kernel Preemption: NONE, VOLUNTARY, PREEMPT, and PREEMPT_RT

> **Subsystem:** Scheduling · **Files:** `kernel/sched/core.c`, `include/linux/preempt.h`, `kernel/Kconfig.preempt`
> **Interviewer is really probing:** Do you understand **when the kernel can switch tasks**, the
> latency/throughput trade-off of each model, and what **PREEMPT_RT** fundamentally changes?

---

## TL;DR Cheat Sheet

- **Preemption** = the ability to **switch away from the currently running task** to a higher-
  priority one. **User-space** is always preemptible; the question is **how preemptible the *kernel*
  is** while a task runs in kernel mode (syscall, fault).
- Four models (latency ↑, throughput slightly ↓ as you go down):
  | Model | Kernel preemptible? | Latency | Use |
  |-------|---------------------|---------|-----|
  | `PREEMPT_NONE` | only at explicit points / return-to-user | highest | servers/throughput |
  | `PREEMPT_VOLUNTARY` | + `might_sleep()` cond_resched points | medium-high | desktops/general |
  | `PREEMPT` (full) | anywhere not holding a lock / preempt-disabled | low | low-latency, desktop, Android |
  | `PREEMPT_RT` | almost everywhere; IRQs threaded, spinlocks sleep | lowest | real-time |
- Preemption is gated by **`preempt_count`**: non-zero (spinlock held, IRQ, explicit
  `preempt_disable`) → **no preemption**. When it returns to zero and **`TIF_NEED_RESCHED`** is set,
  the kernel reschedules.
- **PREEMPT_RT** (now largely mainline): converts **spinlocks → sleeping rt-mutexes**, forces
  **threaded IRQs**, adds **priority inheritance**, shrinks non-preemptible regions to tiny
  `raw_spinlock`/IRQ-off sections → bounded worst-case latency.
- Trade-off: more preemption = **lower latency** but **more context switches / cache churn / slightly
  lower throughput**. Pick per workload.

---

## The Question

> What is preemption? Compare `CONFIG_PREEMPT_NONE`, `PREEMPT_VOLUNTARY`, `PREEMPT`, and `PREEMPT_RT`.

---

## Why preemption models exist

There's a fundamental tension:

- **Throughput** wants a running task to keep the CPU until it naturally blocks or finishes — fewer
  context switches, warmer caches, less overhead. A batch/HPC server prefers this.
- **Latency/responsiveness** wants a newly-runnable high-priority task (a woken audio thread, an RT
  control loop, an interactive UI) to run **immediately**, even if it means yanking the CPU from a
  task currently in the **middle of a syscall**.

The danger: the kernel runs **shared, lock-protected** code. If you preempt a task **while it holds
a spinlock or is in a critical region**, another task could try the same lock and **deadlock** or
corrupt state. So "how preemptible is the kernel" is really "how aggressively can we switch tasks
**while being safe about kernel critical sections**." The four models are **points on that
latency-vs-throughput-vs-complexity curve**, all sharing the same safety rule: **never preempt while
`preempt_count != 0`**.

---

## When preemption can occur

A reschedule happens when **`TIF_NEED_RESCHED`** is set (by a wakeup, tick, or priority change) **and**
preemption is currently allowed:

- **Always-safe points (all models):** returning to **user space**, and **voluntary** blocking
  (`schedule()`, `mutex_lock` that sleeps, `cond_resched()`).
- **`PREEMPT_VOLUNTARY`:** adds reschedule checks at `might_sleep()` / `cond_resched()` sprinkled in
  long kernel loops — bounded but not tiny latency.
- **Full `PREEMPT`:** on **return from interrupt to kernel mode** and whenever `preempt_count` drops
  to zero (e.g. `preempt_enable`, `spin_unlock`) with `NEED_RESCHED` set — i.e. **almost anywhere**
  the kernel isn't in a non-preemptible region.
- **`PREEMPT_RT`:** as full preempt, but the non-preemptible regions are made **as small as
  possible** (most spinlocks become preemptible sleeping locks), so preemption can happen in vastly
  more places.

Never (any model): while holding a **`raw_spinlock`**, in **hardirq/NMI** context, or with
**`preempt_disable()`** / IRQs off — `preempt_count` blocks it.

---

## Where in the kernel

```
kernel/Kconfig.preempt        <- the four config options
include/linux/preempt.h       <- preempt_count, preempt_disable/enable, preempt_enable() reschedule
kernel/sched/core.c           <- preempt_schedule(), preempt_schedule_irq(), __schedule()
include/linux/sched.h         <- TIF_NEED_RESCHED, need_resched()
arch/*/kernel/entry*          <- return-from-IRQ/syscall preemption checks
RT: rtmutex.c, spinlock_rt.h, "PREEMPT_RT" pieces now merged across the tree
```

`preempt_count` (per-task/per-CPU) bitfields: **PREEMPT** (preempt_disable nesting), **SOFTIRQ**,
**HARDIRQ**, **NMI**. Any non-zero field ⇒ not preemptible.

---

## How each model works

### preempt_count: the universal gate

```c
preempt_disable();   /* preempt_count++  -> kernel won't switch tasks here */
... critical section ...
preempt_enable();    /* preempt_count--; if 0 and need_resched(), reschedule NOW */
```
`spin_lock()` implies `preempt_disable()`; `spin_unlock()` implies `preempt_enable()`. So holding any
spinlock makes you non-preemptible — which is **why you must keep spinlock sections short** (a
preempted lock holder would stall everyone, and in non-RT you can't be preempted there anyway).

### PREEMPT_NONE (server)

The kernel **only** reschedules at **explicit** points: return-to-user, voluntary `schedule()`, and
blocking calls. A long in-kernel computation (e.g. a big syscall loop) runs **uninterrupted** until
it hits one of those — **best throughput**, but a high-priority task can wait **milliseconds**.
Default historically for throughput-oriented servers. (Newer kernels can approximate this via the
**dynamic preemption / `preempt=` boot param** and lazy-resched work.)

### PREEMPT_VOLUNTARY (desktop/general)

Adds **`cond_resched()`** / `might_sleep()` checkpoints inside long kernel operations: "if a higher-
priority task is waiting, voluntarily yield here." Reduces worst-case latency vs NONE with **minimal
overhead**, since it only checks at chosen safe points. Common default for general-purpose distros.

### PREEMPT (full / low-latency)

The kernel is preemptible **anywhere `preempt_count == 0`**. On interrupt return to kernel mode, or
when a `preempt_enable`/`spin_unlock` drops the count to zero with `NEED_RESCHED` set, it **switches
immediately**. Latency drops to the length of the **longest non-preemptible region** (a spinlock
hold or IRQ-disabled stretch). Costs more context switches and some throughput. Used for desktops,
Android, multimedia.

### PREEMPT_RT (real-time)

Goes after the remaining sources of unbounded latency — the **non-preemptible regions themselves**:

1. **Spinlocks become sleeping locks:** most `spinlock_t` are converted to **PI-aware rt-mutexes**,
   so holding one is **preemptible** (you can be switched out while "holding a lock"). Truly atomic
   spots use **`raw_spinlock_t`**, which stay non-preemptible and must be tiny.
2. **Threaded IRQs everywhere:** hardirq handlers run in **kthreads** (Q11/Q13), so interrupts don't
   create long non-preemptible windows and can be **prioritized/preempted**.
3. **Priority inheritance:** rt-mutexes implement **PI** so a high-priority task blocked on a lock
   held by a low-priority task **boosts** the holder, preventing **priority inversion** (the Mars
   Pathfinder bug).
4. **Threaded softirqs**, sleeping `local_lock`, etc.

Result: worst-case scheduling latency becomes **small and bounded** (microseconds), enabling **hard
real-time** on Linux. Cost: more context switches, more overhead, careful driver requirements. Most
of PREEMPT_RT is now **merged into mainline** and selectable.

---

## Diagrams

### Latency vs throughput spectrum

```
throughput  <==============================================>  latency
 PREEMPT_NONE     PREEMPT_VOLUNTARY      PREEMPT(full)      PREEMPT_RT
 (reschedule      (+cond_resched         (anywhere          (+spinlocks sleep,
  at few points)   checkpoints)           preempt_count==0)  threaded IRQs, PI)
```

### Preemption decision

```mermaid
flowchart TD
    A[NEED_RESCHED set by wakeup/tick] --> B{preempt_count == 0?}
    B -- no (spinlock/IRQ/disable) --> W[wait until it reaches 0]
    B -- yes --> C{model allows preemption here?}
    C -- NONE: only explicit points --> D[reschedule at next return-to-user/cond_resched]
    C -- VOLUNTARY: at cond_resched --> E[reschedule at next checkpoint]
    C -- PREEMPT/RT: now --> F[__schedule(): switch immediately]
```

---

## Annotated C

```c
/* The gate every model shares. */
#define preempt_count_inc()  /* disable preemption */
#define preempt_count_dec_and_test()  /* enable; true if should resched */

static inline void preempt_enable(void) {
    barrier();
    if (unlikely(preempt_count_dec_and_test() && need_resched()))
        __preempt_schedule();   /* full PREEMPT: switch now */
}

/* Voluntary checkpoint (VOLUNTARY model relies on these in long loops). */
for (i = 0; i < huge; i++) {
    do_work(i);
    cond_resched();   /* yield if a higher-prio task is waiting (safe point) */
}

/* PREEMPT_RT: this "spinlock" may SLEEP (it's an rt-mutex); raw_spinlock does not. */
spinlock_t      lock;       /* RT: sleeping, PI-aware, preemptible while held */
raw_spinlock_t  raw_lock;   /* RT: truly atomic, non-preemptible, keep tiny    */

/* Reschedule on IRQ return to kernel (full preempt/RT path). */
asmlinkage void preempt_schedule_irq(void);
```

> Senior nuance: under **PREEMPT_RT** the mental model flips — `spin_lock()` can **block**, so code
> that assumed "spinlock ⇒ atomic ⇒ can't sleep" must use **`raw_spinlock_t`** where true atomicity
> is required. Misusing a sleeping spinlock in genuinely-atomic context is an RT-specific bug class.

---

## Company Angle

- **Qualcomm/NVIDIA (RT/embedded/automotive):** **PREEMPT_RT** is central — bounded latency for
  control loops, audio, automotive; PI to avoid priority inversion; `raw_spinlock` discipline;
  threaded IRQs (Q11). Android ships **full PREEMPT** for UI responsiveness.
- **Google (servers vs latency):** choosing `NONE`/`VOLUNTARY` for throughput fleets vs `PREEMPT`
  for latency-sensitive services; **dynamic preemption** (`preempt=` at boot) to tune without
  rebuilding; tail-latency impact of non-preemptible kernel regions.
- **AMD (throughput):** preemption overhead vs throughput on many-core; context-switch and cache
  churn costs of full preemption.

---

## War Story

*"A robotics control thread needed a **sub-millisecond** worst-case wakeup latency, but on a stock
`PREEMPT_VOLUNTARY` kernel we saw occasional **multi-millisecond** spikes. `cyclictest` confirmed the
worst-case, and `ftrace`'s `wakeup_rt` / `preemptirqsoff` tracers pinned the culprit: a driver held a
**spinlock with IRQs disabled** for a long stretch during a bulk register operation — a
non-preemptible region that delayed our RT thread. Two-part fix: (1) we moved to a **PREEMPT_RT**
kernel so most spinlocks became preemptible and IRQs were threaded, collapsing the baseline
worst-case; (2) the offending driver still had a genuine `raw_spinlock`/IRQ-off section that was too
long, so we **broke it into smaller chunks** to bound that atomic window. `cyclictest` worst-case
dropped from milliseconds to tens of microseconds. The teaching point: **PREEMPT_RT shrinks
non-preemptible regions, but you still have to keep your `raw_spinlock`/IRQ-off sections short** —
RT exposes them, it doesn't magically remove them."*

---

## Interviewer Follow-ups

1. **What is kernel preemption?** The ability to switch away from a task **executing in kernel mode**
   to a higher-priority task, not just at return-to-user.

2. **What prevents preemption?** Non-zero `preempt_count`: holding a spinlock/`raw_spinlock`,
   `preempt_disable()`, hardirq/softirq/NMI context, IRQs disabled.

3. **NONE vs VOLUNTARY?** NONE reschedules only at explicit points (best throughput, worst latency);
   VOLUNTARY adds `cond_resched()` checkpoints in long loops for bounded latency at low cost.

4. **What does full PREEMPT change?** Kernel becomes preemptible anywhere `preempt_count == 0`,
   including IRQ-return-to-kernel; latency bounded by the longest non-preemptible region.

5. **What does PREEMPT_RT fundamentally do?** Converts most spinlocks to **sleeping PI rt-mutexes**,
   **threads IRQs/softirqs**, and minimizes `raw_spinlock`/IRQ-off regions → bounded, tiny worst-case
   latency.

6. **spinlock_t vs raw_spinlock_t under RT?** `spinlock_t` may sleep (preemptible); `raw_spinlock_t`
   stays truly atomic/non-preemptible — use it only for tiny, must-be-atomic regions.

7. **What is priority inversion and how does RT fix it?** A high-prio task blocked on a lock held by
   a low-prio task; **priority inheritance** boosts the holder to release it quickly.

8. **Trade-off of more preemption?** Lower latency but more context switches, cache churn, and
   slightly lower throughput — choose per workload.

9. **Dynamic preemption?** Modern kernels can switch models at **boot** (`preempt=none|voluntary|full`)
   or even runtime via static branches, avoiding rebuilds.

---

## 30-Minute Talk Track

| Min | Cover |
|-----|-------|
| 0–4 | Define preemption; user always preemptible; question is kernel-mode preemptibility |
| 4–8 | The safety rule: preempt_count gate; spinlock implies preempt_disable |
| 8–12 | PREEMPT_NONE: explicit points only; throughput; latency cost |
| 12–15 | PREEMPT_VOLUNTARY: cond_resched/might_sleep checkpoints |
| 15–19 | Full PREEMPT: anywhere preempt_count==0, IRQ-return preemption, latency bound |
| 19–25 | PREEMPT_RT: sleeping spinlocks, threaded IRQs, PI, raw_spinlock discipline |
| 25–28 | Latency-vs-throughput spectrum; dynamic preemption (preempt= boot) |
| 28–30 | War story (cyclictest + RT + shrink raw_spinlock) + summary |
