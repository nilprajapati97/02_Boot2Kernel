# Q14 — The CFS Scheduler and Its Successor EEVDF

> **Subsystem:** Scheduling · **Files:** `kernel/sched/fair.c`, `kernel/sched/core.c`, `include/linux/sched.h`
> **Interviewer is really probing:** Do you understand **fair scheduling** via virtual runtime,
> the **RB-tree pick**, **nice/weight**, and the **EEVDF** model that replaced CFS in 6.6?

---

## TL;DR Cheat Sheet

- **CFS (Completely Fair Scheduler)** models an **ideal multitasking CPU** where every runnable task
  gets an equal share. It tracks **`vruntime`** (virtual runtime) per task: time actually run,
  **scaled by the task's weight** (from `nice`). **Lowest `vruntime` runs next** → fairness.
- Runnable tasks live in a **per-CPU red-black tree** keyed by `vruntime`; the **leftmost** node
  (smallest vruntime) is the next to run. Pick = O(1) (cached leftmost), insert = O(log n).
- **`nice`** (−20..+19) maps to a **weight**; higher priority = bigger weight = `vruntime` advances
  **slower** = more CPU. A +1 nice step ≈ **10% CPU** difference.
- No fixed timeslices: CFS computes a **dynamic slice** from a target **scheduling latency** divided
  among runnable tasks (with a `min_granularity` floor) so fairness holds at fine granularity.
- **EEVDF (Earliest Eligible Virtual Deadline First)** replaced CFS as the default fair scheduler in
  **Linux 6.6**. It adds **latency-awareness**: each task has a **virtual deadline** derived from a
  requested **time slice / latency-nice**; the scheduler picks the **eligible** task with the
  **earliest virtual deadline**, improving latency for interactive tasks while preserving fairness.
- Other classes coexist via **sched_class** priority: `stop > deadline (SCHED_DEADLINE) > rt
  (SCHED_FIFO/RR) > fair (CFS/EEVDF) > idle`.

---

## The Question

> Explain the CFS scheduler — vruntime, red-black tree, nice values, and load balancing. Bonus:
> discuss the newer EEVDF scheduler that replaced CFS in 6.6+.

---

## Why CFS/EEVDF exists

Earlier schedulers (the O(1) scheduler) used **heuristics** to guess interactivity and assign
priority bonuses — complex, gameable, and unfair in corner cases. The goal of CFS was a **simple,
principled fairness model**: pretend there's an **ideal CPU** that runs **all N runnable tasks
simultaneously at 1/N speed**. Real hardware can only run one at a time, so CFS **approximates** that
ideal by always running whichever task is **furthest behind** its fair share — measured by
**`vruntime`**. This is fair *by construction*, needs no interactivity heuristics, and weights cleanly
by priority.

**Why EEVDF then?** CFS is fair on **throughput** (everyone gets their share *eventually*) but has
weaker control over **latency** — *when* a task gets to run within a period. Interactive/latency-
sensitive tasks (audio, UI, RPC handlers) want to run **soon**, not just **eventually**. EEVDF keeps
CFS's fairness but adds an explicit **deadline** so it can prefer tasks that need low latency,
configurable per-task via **latency-nice**. It's a cleaner theoretical model (eligibility +
virtual deadlines) that subsumes what CFS did with ad-hoc wakeup-preemption tweaks.

---

## When the scheduler runs / picks

- On **tick** (`scheduler_tick()`): update current task's `vruntime`/runtime, check if it should be
  preempted (its vruntime passed a competitor's, or EEVDF deadline elapsed).
- On **wakeup**: a newly-runnable task may **preempt** the current one if it's more deserving
  (smaller vruntime / earlier eligible deadline) — this is **wakeup preemption**, key to latency.
- On **block/yield/exit**: current task leaves the runqueue; pick the next (leftmost).
- On **`schedule()`** from any voluntary point. Governed by preemption model (Q16).

---

## Where in the kernel

```
kernel/sched/fair.c     <- CFS/EEVDF: enqueue/dequeue, pick_next, vruntime, RB-tree, load balance
kernel/sched/core.c     <- __schedule(), context_switch(), runqueues (struct rq)
kernel/sched/rt.c       <- SCHED_FIFO/RR
kernel/sched/deadline.c <- SCHED_DEADLINE (EDF)
include/linux/sched.h   <- task_struct, sched_entity (se), load_weight
struct rq (per-CPU runqueue) embeds struct cfs_rq (the fair runqueue + RB-tree)
```

Each task carries a **`struct sched_entity se`** with its `vruntime`, `load.weight`, and (EEVDF)
eligibility/deadline fields. Group scheduling (cgroups) nests `cfs_rq`s hierarchically.

---

## How it works — mechanics

### CFS: vruntime is the heart

- When a task runs for `delta` real nanoseconds, its `vruntime` increases by
  `delta * NICE_0_WEIGHT / task_weight`. A **high-priority** task (large weight) accrues vruntime
  **slowly** → it sits at the left of the tree longer → gets the CPU more often. A low-priority task
  accrues vruntime fast → drifts right → runs less.
- **Pick next:** the task with the **smallest vruntime** (leftmost RB-tree node, cached in
  `rb_leftmost`) — it's the one most "behind" its fair share.
- **New/woken tasks:** their vruntime is set near the runqueue's `min_vruntime` (not 0) so they
  don't unfairly dominate after sleeping (no infinite "catch-up").

### Weights and nice

`nice` (−20..+19) indexes a **`sched_prio_to_weight[]`** table. Each nice step changes weight by
~**1.25×**, yielding the well-known **~10% CPU per nice level**. CPU share ≈ `task_weight /
sum_of_weights` of runnable tasks. So nice is a **proportional** knob, not absolute.

### Dynamic timeslice (no fixed quantum)

CFS targets a **scheduling latency** (`sysctl_sched_latency`, e.g. ~6 ms) within which every
runnable task should run once. Each task's slice ≈ `latency * weight / total_weight`, floored at
`sched_min_granularity` (to bound context-switch overhead). With many tasks, the period stretches
(`sched_latency` scales by `nr_running` past a threshold).

### The red-black tree

```
        (min_vruntime baseline)
              [ vr=20 ]
             /         \
        [vr=12]        [vr=35]
        /     \
   [vr=8]*   [vr=15]      *leftmost = next to run (smallest vruntime)
```
Self-balancing → O(log n) insert/delete, O(1) leftmost pick (cached). Fairness emerges purely from
the **ordering by vruntime**.

### EEVDF: eligibility + virtual deadline

EEVDF refines the pick using two concepts:
- **Lag / eligibility:** a task is **eligible** if it hasn't run ahead of its fair share (its
  *lag* ≥ 0 — it's owed time). Only eligible tasks compete, which bounds unfairness tightly.
- **Virtual deadline:** each task requests a **time slice** (related to **latency-nice**); its
  **virtual deadline** = the virtual time by which that slice should complete. The scheduler picks
  the **eligible task with the earliest virtual deadline** → tasks that asked for **low latency**
  (short slice, near deadline) get scheduled **promptly**, while fairness (eligibility) is preserved.
- Effect: a latency-sensitive task can be configured to **preempt sooner** and run in **smaller,
  more frequent** slices without stealing more than its fair total share. This replaced a pile of
  CFS heuristics (`GENTLE_FAIR_SLEEPERS`, wakeup-granularity tuning) with one coherent model.
- **latency-nice** (`sched_setattr` `SCHED_FLAG_LATENCY_NICE` / `latency_nice`) lets userspace bias a
  task toward latency (negative) or throughput (positive) **independently** of `nice` (which controls
  CPU share).

### Context switch

`__schedule()` picks next, then `context_switch()` swaps **mm** (if different — TLB/`CR3`/`TTBR`,
Q1) and **CPU registers/stack** (`switch_to`). Per-CPU runqueue locks (`rq->lock`) protect enqueue/
dequeue; balancing moves tasks between CPUs (Q15).

---

## Diagrams

### Pick-next (CFS)

```mermaid
flowchart TD
    T[scheduler_tick / wakeup / schedule] --> U[update curr vruntime += delta*W0/weight]
    U --> C{curr still leftmost / before EEVDF deadline?}
    C -- yes --> K[keep running]
    C -- no --> P[pick leftmost (CFS) / earliest-deadline eligible (EEVDF)]
    P --> SW[context_switch -> new task]
```

### vruntime fairness intuition

```
high-prio (weight 4x): vruntime ticks slowly  ──▏──▏──▏──▏  (runs often)
nice-0   (weight 1x):  vruntime ticks normally ─▏─▏─▏─▏─▏
low-prio (weight .5x): vruntime ticks fast     ▏▏▏▏▏▏▏▏▏▏   (runs less)
Scheduler always runs the one furthest LEFT (smallest vruntime).
```

---

## Annotated C

```c
/* Per-task scheduling entity (the unit CFS/EEVDF schedules). */
struct sched_entity {
    struct load_weight load;     /* weight derived from nice */
    struct rb_node     run_node; /* node in cfs_rq RB-tree, keyed by vruntime */
    u64  vruntime;               /* virtual runtime: the fairness key */
    u64  sum_exec_runtime;       /* real ns run */
    /* EEVDF: */
    s64  vlag;                   /* lag: + = owed time (eligible), - = ran ahead */
    u64  deadline;               /* virtual deadline for current slice */
    u64  slice;                  /* requested time slice (latency-nice influenced) */
};

/* The fair runqueue: RB-tree + cached leftmost + min_vruntime baseline. */
struct cfs_rq {
    struct load_weight load;
    u64 min_vruntime;            /* monotonic floor; new tasks start near here */
    struct rb_root_cached tasks_timeline; /* RB-tree, leftmost cached */
    struct sched_entity *curr;
};

/* vruntime accounting (conceptual): higher weight -> slower vruntime growth. */
static void update_curr(struct cfs_rq *cfs_rq) {
    u64 delta = now - curr->exec_start;
    curr->vruntime += delta * NICE_0_LOAD / curr->load.weight; /* weighted */
    /* EEVDF also updates vlag and checks deadline */
}
```

> Senior nuance: **vruntime is virtual, not wall-clock** — it's deliberately distorted by weight so
> that "equal vruntime = fair share" already accounts for priority. EEVDF adds **lag** (how far
> ahead/behind fair share) and **deadlines** (when, not just how much).

---

## Company Angle

- **Google (scheduling at scale):** flagship area — cgroup CPU controller (`cpu.weight`,
  `cpu.max` bandwidth), `SCHED_DEADLINE` for latency SLOs, EEVDF/latency-nice for tail latency,
  CFS bandwidth throttling for containers, and why heuristics were replaced by EEVDF's model.
- **AMD (NUMA/topology):** interaction with **load balancing** across CCX/dies/NUMA (Q15); per-CPU
  runqueues and migration cost; wakeup placement (`select_task_rq_fair`).
- **NVIDIA/Qualcomm (RT/latency):** `SCHED_FIFO/RR` and `SCHED_DEADLINE` for real-time work,
  PREEMPT_RT, and EEVDF/latency-nice for interactive/audio latency; priority vs fairness trade-offs.

---

## War Story

*"A latency-sensitive RPC server shared cores with batch jobs. Under CFS, the RPC threads got their
**fair share** but suffered p99 spikes — they'd wait up to a full scheduling-latency period behind
several CPU-hungry batch threads before running. `nice` didn't help cleanly: lowering batch `nice`
changed **CPU share** but the RPC threads still weren't scheduled **promptly** on wakeup. On a 6.6
kernel I used **EEVDF latency-nice** (`sched_setattr` with a negative `latency_nice`) on the RPC
threads: they kept the same fair CPU *share* but got **earlier virtual deadlines**, so on wakeup they
**preempted promptly** and ran in **shorter, more frequent** slices. p99 dropped substantially. The
key insight I gave: **`nice` controls *how much* CPU; latency-nice controls *how soon*** — CFS
conflated these, EEVDF separates them."*

---

## Interviewer Follow-ups

1. **What is vruntime?** Per-task virtual runtime = real runtime scaled by weight; the scheduler runs
   the smallest-vruntime task to equalize fair shares.

2. **Why a red-black tree?** O(log n) insert/delete, O(1) leftmost (smallest vruntime) pick; keeps
   tasks ordered by fairness key.

3. **How does nice map to CPU?** Nice→weight table (~1.25× per step); share ≈ weight/Σweights ≈ 10%
   CPU per nice level. Proportional, not absolute.

4. **Why don't sleeping tasks starve others on wakeup?** Their vruntime is clamped near
   `min_vruntime`, preventing infinite catch-up after a long sleep.

5. **What does EEVDF add over CFS?** Explicit **eligibility (lag)** + **virtual deadlines**; picks
   earliest-deadline eligible task → latency control via **latency-nice**, while preserving fairness.

6. **nice vs latency-nice?** `nice` = CPU **share** (bandwidth); `latency-nice` = scheduling
   **promptness/latency** — independent knobs under EEVDF.

7. **Scheduling classes order?** `stop > deadline > rt > fair(CFS/EEVDF) > idle`; higher classes
   always preempt lower ones (an RT task preempts all fair tasks).

8. **How is a timeslice chosen?** Dynamically: target latency / runnable weight share, floored at
   `min_granularity`; period scales with `nr_running`.

9. **What is `SCHED_DEADLINE`?** A separate EDF class for tasks with explicit (runtime, deadline,
   period) reservations — hard latency guarantees above the fair class.

---

## 30-Minute Talk Track

| Min | Cover |
|-----|-------|
| 0–3 | Goal: ideal fair CPU; why heuristic schedulers were replaced |
| 3–8 | vruntime: weighted virtual runtime; smallest-runs-next fairness |
| 8–12 | RB-tree: leftmost pick O(1), insert O(log n), min_vruntime baseline |
| 12–16 | nice→weight, ~10%/level, proportional share; dynamic timeslice/latency |
| 16–18 | wakeup preemption, new/woken task vruntime clamping |
| 18–24 | EEVDF: eligibility/lag + virtual deadlines + latency-nice; why 6.6 switched |
| 24–27 | Scheduling classes, SCHED_DEADLINE/RT coexistence; context_switch |
| 27–30 | War story (latency-nice for RPC) + nice-vs-latency summary |
