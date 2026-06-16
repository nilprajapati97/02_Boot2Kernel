# Q07 — Design a Read-Copy-Update (RCU) System from Scratch

---

## 1. Problem Statement

RCU (Read-Copy-Update) is the most powerful synchronization primitive in the Linux kernel. It allows **unlimited concurrent readers with zero synchronization overhead** on the read side, while writers pay a deferred-reclaim cost instead of a lock acquisition cost.

Design the RCU system from scratch — covering the read-side primitives, grace period detection, callback batching, quiescent state reporting, and the `kthread`-based grace period daemon — with enough depth to explain Linux's `TREE_RCU` implementation.

---

## 2. Requirements

### 2.1 Functional Requirements
- `rcu_read_lock()` / `rcu_read_unlock()` with zero cache-line contention.
- `synchronize_rcu()`: block the calling thread until all pre-existing readers complete.
- `call_rcu()`: register a callback to run after the next grace period (non-blocking).
- `rcu_assign_pointer()` / `rcu_dereference()`: memory-order-correct pointer publish/read.
- `rcu_barrier()`: wait for all outstanding `call_rcu()` callbacks to complete.
- Correct behavior on CPU hotplug (offline CPUs must not delay grace periods).
- `SRCU` (Sleepable RCU): variant where read-side critical sections can sleep.

### 2.2 Non-Functional Requirements
- Read-side overhead: < 2 ns (comparable to `preempt_disable`).
- Grace period latency (no load): < 100 µs.
- Grace period latency (heavily loaded): scales with scheduling latency of CPUs.
- `call_rcu()` callback batching: coalesce 1000s of callbacks per grace period.
- Scalable to 4096 CPUs without O(N) synchronization in the grace period path.

---

## 3. Constraints & Assumptions

- Linux TREE_RCU (the default for SMP systems, CONFIG_TREE_RCU=y).
- Preemptible RCU (`CONFIG_PREEMPT_RCU`): readers can be preempted mid-section.
- Non-preemptible RCU (classic): readers cannot be preempted (simpler, lower overhead).
- We design TREE_RCU (non-preemptible variant) for clarity.

---

## 4. Architecture Overview

```
                        TREE RCU Architecture
  ┌─────────────────────────────────────────────────────────────────┐
  │                     Grace Period Kthread                        │
  │                   (rcu_gp_kthread — one global)                 │
  │                          │                                      │
  │              ┌───────────▼────────────┐                         │
  │              │     RCU Node Tree      │ ← combiner tree         │
  │              │  (hierarchical, O(log N))                        │
  │              │                                                  │
  │              │  Root Node                                       │
  │              │   ├── Level-1 Node (CPUs 0-63)                   │
  │              │   │    ├── CPU 0 rcu_data                        │
  │              │   │    ├── CPU 1 rcu_data                        │
  │              │   │    └── ...                                   │
  │              │   └── Level-1 Node (CPUs 64-127)                 │
  │              └──────────────────────────────────────────────────┘
  │                                                                  │
  │  Per-CPU rcu_data:                                               │
  │    ├── gp_seq_needed    (which GP this CPU needs)               │
  │    ├── qs_pending       (needs quiescent state)                 │
  │    ├── nxtlist          (pending call_rcu callbacks)            │
  │    └── qlen             (callback queue length)                 │
  └─────────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 RCU State (global)

```c
struct rcu_state {
    struct rcu_node     node[NUM_RCU_NODES];    /* combining tree */
    struct rcu_data __percpu *data;              /* per-CPU data */

    unsigned long       gp_seq;       /* grace period sequence number */
    /* gp_seq & 1 == 0: no GP in progress
       gp_seq & 1 == 1: GP in progress */

    struct task_struct *gp_kthread;   /* GP daemon */
    struct swait_queue_head gp_wq;   /* GP kthread waits here */
    bool                gp_flags;    /* RCU_GP_FLAG_INIT, RCU_GP_FLAG_FQS */
};
```

### 5.2 RCU Node (combiner tree)

```c
struct rcu_node {
    raw_spinlock_t  lock;
    unsigned long   gp_seq;         /* GP seq at node level */
    unsigned long   gp_seq_needed;  /* what GP this node needs */
    unsigned long   qsmask;         /* bitmask: which children need QS */
    unsigned long   qsmaskinit;     /* initial qsmask for each GP */
    unsigned long   expmask;        /* expedited GP qsmask */
    struct rcu_node *parent;        /* NULL for root */
    int             grplo, grphi;   /* CPU range this node covers */
    int             grpnum;         /* index within parent's children */
    struct list_head blkd_tasks;    /* preempted tasks in RCU read section */
};
```

### 5.3 Per-CPU RCU Data

```c
struct rcu_data {
    /* Quiescent state tracking */
    unsigned long   gp_seq;          /* last GP seq we reported QS for */
    unsigned long   gp_seq_needed;   /* next GP seq we need */
    bool            cpu_no_qs;       /* needs to pass QS */
    bool            core_needs_qs;   /* GP started, needs QS from this CPU */

    /* Callback list (pending call_rcu callbacks) */
    struct rcu_segcblist cblist;     /* segmented callback list */
    long            qlen;            /* total callbacks pending */

    /* CPU identity */
    int             cpu;
    struct rcu_node *mynode;         /* leaf node in combiner tree */
    unsigned long   grpmask;         /* this CPU's bit in rcu_node.qsmask */
};
```

### 5.4 Segmented Callback List (`rcu_segcblist`)

Callbacks are organized into segments based on which grace period they belong to:
```c
struct rcu_segcblist {
    struct rcu_head *head;     /* pointer to oldest callback */
    struct rcu_head **tails[RCU_CBLIST_NSEGS]; /* segment tail pointers */
    /*
     * Segments:
     *   [0] RCU_DONE_TAIL: callbacks ready to invoke (GP has passed)
     *   [1] RCU_WAIT_TAIL: waiting for current GP
     *   [2] RCU_NEXT_READY_TAIL: waiting for next GP to start
     *   [3] RCU_NEXT_TAIL: just registered
     */
    long            len;
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Read-Side Critical Section (Non-Preemptible TREE_RCU)

```c
static inline void rcu_read_lock(void)
{
    __rcu_read_lock();   /* == preempt_disable() */
    /* That's it. No cache line touched, no atomic operation. */
}

static inline void rcu_read_unlock(void)
{
    __rcu_read_unlock();  /* == preempt_enable() */
    /* A preempt_enable() schedules if needed, which IS a quiescent state. */
}
```

**Why preempt_disable is sufficient:** In non-preemptible RCU, a CPU passes through a quiescent state whenever it:
- Executes with preemption enabled (is schedulable)
- Goes idle (`cpu_idle()`)
- Executes in user space
- Executes an explicit `schedule()` call

A CPU inside `rcu_read_lock()` has preemption disabled → it cannot be scheduled → the scheduler cannot observe a context switch on that CPU → the grace period waits for it.

### 6.2 Grace Period Detection Algorithm

A grace period is the interval during which every CPU passes through at least one quiescent state.

```
GP START:
  gp_kthread wakes up (signaled by call_rcu or synchronize_rcu)
  rcu_gp_init():
    lock root rcu_node
    increment gp_seq (gp_seq | 1 == GP in progress)
    set qsmask on all rcu_nodes = qsmaskinit (all CPUs need to report QS)
    unlock

GP WAIT:
  For each CPU in the tree:
    - scheduler tick calls rcu_check_callbacks()
    - if CPU was in user space or idle since GP start: report QS
      rdp->cpu_no_qs = false
      rcu_report_qs_rdp()
        → clears bit in leaf rcu_node.qsmask
        → if qsmask == 0: propagate up to parent node
        → root qsmask == 0: all CPUs reported QS → GP can end

GP END:
  gp_seq += 1 (gp_seq & 1 == 0 → no GP in progress)
  invoke_rcu_callbacks():
    for each CPU: move RCU_WAIT_TAIL segment to RCU_DONE_TAIL
    schedule softirq (RCU_SOFTIRQ) to invoke callbacks
```

### 6.3 Force-Quiescent-State (FQS)

If a CPU is running a tight loop in kernel space with preemption disabled, it will never voluntarily report a quiescent state. The GP kthread runs a **Force Quiescent State** scan:

```c
rcu_gp_fqs():
    for each CPU with cpu_no_qs set:
        if (cpu_is_idle(cpu)):
            rcu_report_qs_rdp(cpu)  /* idle is always a QS */
        else if (cpu_in_userspace(cpu)):
            rcu_report_qs_rdp(cpu)  /* user space is always a QS */
        else:
            /* CPU is in kernel: send IPI to force a context check */
            smp_call_function_single(cpu, rcu_ipi_qs, NULL, 0)
```

FQS runs every `jiffies_till_sched_qs` jiffies (default ~100 ms).

### 6.4 Hierarchical Combiner Tree (TREE_RCU Scalability)

For 4096 CPUs:
- **Flat scan:** O(4096) work to check all CPUs — holds root lock for too long.
- **Tree scan:** O(log₄ 4096 = 6 levels) — each level aggregates children's QS bits.

Tree structure:
```
Level 0 (leaves): 1 rcu_node per RCU_FANOUT (default 16 CPUs)
Level 1:          1 rcu_node per RCU_FANOUT² = 256 CPUs
Level 2 (root):   1 rcu_node covering all CPUs
```

When a leaf node's `qsmask` hits 0, it propagates up by clearing its bit in the parent's `qsmask`. The root node reaching `qsmask == 0` ends the grace period.

### 6.5 call_rcu() — Asynchronous Callback Registration

```c
void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
    rdp = this_cpu_ptr(&rcu_data);
    /* Append to RCU_NEXT_TAIL segment — locked via local_irq_save */
    local_irq_save(flags);
    rcu_segcblist_enqueue(&rdp->cblist, head);
    local_irq_restore(flags);

    /* If GP not in progress: kick GP kthread to start one */
    if (!rcu_gp_in_progress())
        rcu_start_gp();
}
```

### 6.6 SRCU (Sleepable RCU) — for Read Sections That Sleep

Standard RCU read sections cannot sleep (preemption is disabled). SRCU allows sleeping read sections by using a **per-SRCU-domain per-CPU counter pair**:

```c
struct srcu_struct {
    struct srcu_node  node[NUM_SRCU_NODES];
    struct srcu_data __percpu *sda;
    unsigned long     srcu_gp_seq;
    struct delayed_work work;  /* GP advancement */
};

/* Read side: */
int idx = srcu_read_lock(&my_srcu_domain);   /* idx = 0 or 1 (flip-flop) */
    /* can sleep here */
srcu_read_unlock(&my_srcu_domain, idx);

/* Write side: */
synchronize_srcu(&my_srcu_domain);  /* waits for all readers on both idx */
```

SRCU maintains two per-CPU counters (a flip-flop). Grace periods alternate between the two. Writers wait for the counter of the **previous** index to drain to zero.

---

## 7. Trade-off Analysis

| Aspect | TREE_RCU | SRCU | RCU-sched |
|---|---|---|---|
| Read-side cost | `preempt_disable` only | Increment per-CPU counter | Same as TREE_RCU |
| Readers can sleep | No | Yes | No |
| GP latency | Scheduling-latency bounded | Can be longer (waiters may sleep) | Same |
| Use case | General kernel data structures | File system ops, driver callbacks | RCU for !preempt sections |
| Nested allowed | Yes (via nesting counter) | Yes | No |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| RCU tree core | `kernel/rcu/tree.c` | `rcu_gp_kthread()`, `rcu_gp_init()` |
| Read side | `include/linux/rcupdate.h` | `rcu_read_lock()`, `rcu_dereference()` |
| QS reporting | `kernel/rcu/tree.c` | `rcu_check_callbacks()`, `rcu_report_qs_rdp()` |
| Callback list | `kernel/rcu/rcu_segcblist.c` | `rcu_segcblist_enqueue()` |
| SRCU | `kernel/rcu/srcutree.c` | `srcu_read_lock()`, `synchronize_srcu()` |
| FQS | `kernel/rcu/tree.c` | `rcu_gp_fqs_loop()` |
| RCU callbacks | `kernel/rcu/tree.c` | `rcu_do_batch()`, `invoke_rcu_callbacks()` |
| Memory ordering | `include/linux/rcupdate.h` | `rcu_assign_pointer()`, `rcu_dereference_raw()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 RCU Stall — CPU Stuck in Read Section
```bash
# "INFO: rcu_sched self-detected stall on CPU X"
# Appears when GP cannot complete for > 21 seconds
dmesg | grep "rcu_sched"
# Backtrace of stalled CPU shows where it's stuck
# Common causes: infinite loop with preempt_disable, long hardirq
```

### 9.2 Use-After-Free via Missed synchronize_rcu
```bash
# CONFIG_PROVE_RCU: checks that rcu_dereference() is inside rcu_read_lock()
# CONFIG_RCU_STRICT_GRACE_PERIOD: makes GP very short, exposes races quickly
# KASAN: detects the actual memory access after kfree
```

### 9.3 Callback Flood (call_rcu storm)
```bash
cat /sys/kernel/debug/rcu/rcudata   # per-CPU callback queue lengths
# If qlen >> 1000: writers are faster than GPs
# Fix: use synchronize_rcu() instead of call_rcu() in write path
#      or batch writes: update N objects, then call synchronize_rcu() once
```

---

## 10. Performance Considerations

- **Read-side = preempt_disable:** On an uncontended CPU, this is ~1 ns. There is literally no contention — no shared cache line is written.
- **GP batching:** Multiple `call_rcu()` callbacks registered during one GP all fire after the same GP completes — amortizes GP overhead across many writers.
- **RCU_NOCB CPUs:** On systems with isolated CPUs (GPU processing nodes), `RCU_NOCB` offloads callback processing to a `kthread` — preventing callbacks from interrupting the isolated CPU.
- **Expedited GP:** `synchronize_rcu_expedited()` forces immediate QS via IPI to all CPUs — O(NR_CPUS) IPIs but completes in < 100 µs. Use only for rare, latency-critical paths (driver init/teardown).

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. Quiescent state definition — every CPU not in an RCU read section is a QS (context switch, idle, user space).
2. Tree structure for scalability — O(log N) vs O(N) for 4096 CPUs.
3. `call_rcu()` vs `synchronize_rcu()` — async deferred callback vs synchronous blocking wait.
4. Segmented callback list — callbacks organized by which GP they belong to.
5. SRCU for sleepable critical sections — flip-flop counter design.
6. RCU stall detection and debugging — 21-second timeout, FQS mechanism.
7. `rcu_assign_pointer` / `rcu_dereference` memory ordering — crucial for correctness.
8. `RCU_NOCB` for isolated CPUs — directly relevant to NVIDIA's GPU-isolated CPU configurations.
