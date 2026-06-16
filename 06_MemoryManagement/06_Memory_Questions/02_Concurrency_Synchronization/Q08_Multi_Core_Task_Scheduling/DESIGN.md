# Q08 — Design a Multi-Core Task Scheduling System with Load Balancing

---

## 1. Problem Statement

Modern systems run 100s of threads across 512 cores spanning multiple NUMA nodes and SMT siblings. The scheduler must make O(1) decisions for every context switch (happening millions of times/second) while simultaneously ensuring:
- No CPU is idle while another is overloaded.
- Thread placement respects cache topology (L1/L2/L3 sharing, NUMA).
- Real-time tasks get hard latency guarantees.
- GPU-submitter threads wake on GPU-adjacent CPUs (see Q01).

Design the multi-core scheduling and load balancing subsystem.

---

## 2. Requirements

### 2.1 Functional Requirements
- Per-CPU run queues with O(log N) pick-next (CFS red-black tree).
- Periodic load balancing across CPU topology (SMT > MC > NUMA domains).
- Idle load balancing: pull work to idle CPUs immediately.
- Wake affinity: wake a sleeping task on the CPU it last ran on (cache warmth).
- CPU pinning via `sched_setaffinity()` / `cpuset` cgroups.
- CPU topology awareness: NUMA node, physical core (SMT sibling), LLC domain.

### 2.2 Non-Functional Requirements
- Context switch time: < 2 µs.
- Load balancer overhead: < 1% of total CPU time.
- Idle CPU finds work within one tick (4 ms default, 1 ms with `CONFIG_HZ_1000`).
- NUMA misplacement rate < 5% under steady-state workloads.

---

## 3. Constraints & Assumptions

- Linux CFS scheduler (Completely Fair Scheduler) + SCHED_DEADLINE.
- `CONFIG_SMP=y`, `CONFIG_NUMA=y`, `CONFIG_HZ=1000`.
- x86-64 with CPU topology exposed via `CPUID` and ACPI SRAT table.
- Per-CPU run queue (`struct rq`) — one per logical CPU.

---

## 4. Architecture Overview

```
  CPU Topology (example: 2 NUMA nodes, 4 cores each, 2 SMT siblings)

  NUMA Node 0                        NUMA Node 1
  ┌──────────────────────┐           ┌──────────────────────┐
  │  Core 0    Core 1    │           │  Core 4    Core 5    │
  │  [CPU0,1] [CPU2,3]  │           │  [CPU8,9] [CPU10,11] │
  │  Core 2    Core 3    │           │  Core 6    Core 7    │
  │  [CPU4,5] [CPU6,7]  │           │  [CPU12,13][CPU14,15]│
  └──────────────────────┘           └──────────────────────┘
         LLC shared                         LLC shared
  ──────────────────────────────────────────────────────────

  Scheduling Domains (per CPU, built from topology):
  CPU0 ──► SD_SMT (CPUs 0,1) ──► SD_MC (CPUs 0-7) ──► SD_NUMA (CPUs 0-15)

  Per-CPU Run Queue (struct rq):
  ┌─────────────────────────────────────────┐
  │  cfs_rq:  rb-tree of sched_entities     │  ← CFS tasks
  │  rt_rq:   linked list by prio           │  ← RT tasks (FIFO/RR)
  │  dl_rq:   rb-tree by deadline           │  ← DEADLINE tasks
  │  curr:    currently running task        │
  │  idle:    idle task (always runnable)   │
  │  clock:   rq wall clock (ns)            │
  │  nr_running: count of runnable tasks    │
  │  load:    load weight (for balancing)   │
  └─────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Per-CPU Run Queue

```c
struct rq {
    raw_spinlock_t      lock;

    /* CFS (SCHED_NORMAL, SCHED_BATCH, SCHED_IDLE) */
    struct cfs_rq       cfs;

    /* Real-Time (SCHED_FIFO, SCHED_RR) */
    struct rt_rq        rt;

    /* Deadline (SCHED_DEADLINE) */
    struct dl_rq        dl;

    /* Current task */
    struct task_struct *curr, *idle, *stop;

    /* Load tracking */
    unsigned long       cpu_load[CPU_LOAD_IDX_MAX]; /* EWMA load history */
    struct sched_avg    avg_idle;  /* avg time spent idle */

    /* Clock */
    u64                 clock;             /* raw monotonic ns */
    u64                 clock_task;        /* task-side clock */

    /* Scheduling domains */
    struct sched_domain *sd;               /* innermost domain */

    int                 cpu;
    int                 online;
    unsigned int        nr_running;
};
```

### 5.2 CFS Run Queue — Red-Black Tree

```c
struct cfs_rq {
    struct load_weight  load;            /* total weight of all tasks */
    unsigned int        nr_running;
    u64                 min_vruntime;    /* minimum vruntime in tree */

    struct rb_root_cached tasks_timeline; /* RB-tree: sorted by vruntime */
    struct sched_entity *curr;           /* currently executing entity */
    struct sched_entity *next;           /* will run next (set by wakeup) */
    struct sched_entity *last;           /* last ran (set on preempt) */

    /* For cgroup CPU accounting */
    struct task_group   *tg;
    struct cfs_bandwidth *cfs_bandwidth; /* CPU quota/throttling */
};
```

### 5.3 Scheduler Entity (task's CFS state)

```c
struct sched_entity {
    struct load_weight  load;       /* weight = 1024 * 1.25^(20-nice) */
    struct rb_node      run_node;   /* node in cfs_rq.tasks_timeline */
    struct list_head    group_node;
    unsigned int        on_rq;

    u64                 exec_start;   /* start of current execution */
    u64                 sum_exec_runtime; /* total CPU time used */
    u64                 vruntime;     /* virtual runtime (weighted) */
    u64                 prev_sum_exec_runtime;

    struct sched_avg    avg;          /* PELT: per-entity load tracking */
};
```

### 5.4 Scheduling Domain

```c
struct sched_domain {
    struct sched_domain *parent;      /* larger domain above */
    struct sched_domain *child;       /* smaller domain below */
    struct sched_group  *groups;      /* groups at this level */

    unsigned long        min_interval; /* load balance interval (ms) */
    unsigned long        max_interval;
    unsigned int         busy_factor;

    unsigned int         flags;       /* SD_LOAD_BALANCE, SD_BALANCE_WAKE,
                                         SD_SHARE_PKG_RESOURCES (LLC),
                                         SD_NUMA */
    int                  level;       /* 0=SMT, 1=MC, 2=DIE, 3=NUMA */

    /* Load balance statistics */
    unsigned int         nr_balance_failed;
    u64                  last_balance;
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 CFS Virtual Runtime (vruntime)

CFS schedules the task with the **smallest vruntime** (leftmost node in the RB-tree). Virtual runtime accumulates CPU time weighted by task priority:

$$v_{runtime} += \frac{\Delta t_{real} \times NICE\_0\_WEIGHT}{task.weight}$$

A nice -20 task (highest priority, weight = 88761) accumulates vruntime ~88× slower than a nice 19 task (weight = 15). So a high-priority task appears to have "run less" → CFS schedules it more often.

```c
static void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    u64 now = rq_clock_task(rq_of(cfs_rq));
    u64 delta_exec = now - curr->exec_start;

    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;

    /* vruntime += delta_exec * NICE_0_WEIGHT / curr->load.weight */
    curr->vruntime += calc_delta_fair(delta_exec, curr);

    /* Update cfs_rq.min_vruntime (monotonically increasing) */
    update_min_vruntime(cfs_rq);
}
```

### 6.2 Load Balancing — Periodic (tick-based)

Every `scheduler_tick()` (every 1 ms on HZ=1000):
```
trigger_load_balance(rq):
    if time to balance at this domain level:
        raise SCHED_SOFTIRQ → run_rebalance_domains() in softirq context

run_rebalance_domains():
    for each domain from innermost to outermost:
        load_balance(this_cpu, rq, sd, CPU_IDLE or CPU_NOT_IDLE)

load_balance(this_cpu, rq, sd, idle):
    find_busiest_group(sd) → finds the sched_group with highest load imbalance
    find_busiest_queue(busiest_group) → most loaded CPU in that group
    migrate_tasks(busiest_queue, this_rq, nr_to_move)
```

### 6.3 Idle Load Balancing — Newidle (critical for latency)

When a CPU becomes idle:
```
pick_next_task() → no tasks → newidle_balance():
    /* immediately pull from busiest sibling CPU */
    for sd in domains (innermost first):
        load_balance(cpu, rq, sd, CPU_NEWLY_IDLE)
        if migrated at least one task: break
```

This is the **most latency-sensitive** path — an idle CPU should find work in < 100 µs.

### 6.4 Wake Affinity (Cache-Warm Wakeup)

When task T wakes up (e.g., after `wait_event()`):
```
select_task_rq_fair(T):
    prev_cpu = T->wake_cpu  /* CPU T last ran on */
    this_cpu = current CPU (the waker's CPU)

    /* If T was last awoken from this_cpu's LLC domain: prefer this_cpu */
    if (cpumask_test_cpu(prev_cpu, &sd_llc->span) &&
        !cpu_overloaded(this_cpu)):
        return this_cpu   /* wake on waker's CPU: cache-warm for waker */

    /* Otherwise: wake on prev_cpu if it's not overloaded */
    if (!cpu_overloaded(prev_cpu)):
        return prev_cpu   /* wake on previous CPU: cache-warm for wakee */
```

**Trade-off:** Waking on the waker's CPU improves **producer–consumer locality** (producer's data is in cache). Waking on prev_cpu improves **task's own working set locality**. Linux uses a heuristic based on `wake_affinity_weight`.

### 6.5 NUMA-Aware Load Balancing

`SD_NUMA` domain handles cross-NUMA balancing:
1. **Periodic:** Every `sd_numa->balance_interval` ms, migrate tasks from over-loaded NUMA nodes.
2. **Automatic NUMA (autonuma):** Every `numa_balancing_scan_period_ms`, unmap pages → fault → check if faulting CPU is local to page → if not, migrate page to local node.

NUMA imbalance metric:
```
imbalance = (node_load[remote] - node_load[local]) / 2
if imbalance > threshold: migrate ceil(imbalance / task_weight) tasks
```

### 6.6 SCHED_DEADLINE — Earliest Deadline First (EDF)

For real-time GPU submission threads:
```c
/* Set task as SCHED_DEADLINE */
struct sched_attr attr = {
    .sched_policy   = SCHED_DEADLINE,
    .sched_runtime  = 1000000,   /* 1 ms worst-case execution */
    .sched_deadline = 5000000,   /* 5 ms deadline */
    .sched_period   = 5000000,   /* 5 ms period */
};
sched_setattr(0, &attr, 0);
```

EDF guarantees: task runs within its deadline as long as total utilization ≤ 1.0 per CPU. The kernel admits a new DEADLINE task only if `∑(runtime/period) ≤ (1 - 1/NR_CPUS)` (bandwidth cap for non-deadline tasks).

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Per-CPU run queue | Yes | Global run queue | Per-CPU avoids global lock; scales to 1000s of cores |
| CFS RB-tree | O(log N) pick-next | O(1) active/expired arrays | RB-tree handles arbitrary weights; O(1) only works for discrete priorities |
| Load balance in softirq | Yes | Dedicated lb thread | Softirq runs on same CPU: no context switch overhead for lb itself |
| Wake affinity | LLC-domain preference | Always wake on waker CPU | LLC domain captures memory sharing without cross-NUMA penalty |
| NUMA balance | Soft (page migration) | Hard (task migration) | Page migration preserves task affinity; task migration is more disruptive |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| CFS scheduler | `kernel/sched/fair.c` | `enqueue_task_fair()`, `pick_next_task_fair()`, `update_curr()` |
| Load balancing | `kernel/sched/fair.c` | `load_balance()`, `find_busiest_group()` |
| Idle balance | `kernel/sched/fair.c` | `newidle_balance()` |
| Sched domains | `kernel/sched/topology.c` | `build_sched_domains()`, `sched_domain_topology` |
| DEADLINE sched | `kernel/sched/deadline.c` | `dl_task_timer()`, `enqueue_task_dl()` |
| Wake affinity | `kernel/sched/fair.c` | `select_task_rq_fair()`, `wake_affine()` |
| PELT (load avg) | `kernel/sched/pelt.c` | `update_load_avg()` |
| Per-CPU rq | `kernel/sched/core.c` | `struct rq`, `cpu_rq(cpu)` |
| sched_setattr | `kernel/sched/core.c` | `sched_setattr()`, `__setscheduler_params()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 CPU Imbalance (Hot Spots)
```bash
# Check per-CPU task counts:
cat /proc/schedstat   # per-CPU: nr_switches, run_queue_length
# Or use:
perf stat -a -e sched:sched_migrate_task sleep 10
```

### 9.2 NUMA Misplacement
```bash
numastat -p <pid>   # shows NUMA hits vs misses for process
# High numa_miss: tasks or pages on wrong node
cat /proc/<pid>/numa_maps  # per-VMA NUMA placement
```

### 9.3 Scheduler Latency Spike (context switch delay)
```bash
perf sched record -- sleep 5
perf sched latency   # shows max/avg scheduling latency per task
# Look for outliers: task waiting > 1ms to be scheduled
```

### 9.4 DEADLINE Admission Failure
```bash
# sched_setattr() returns -EBUSY when bandwidth exceeded
# Check current DEADLINE utilization:
cat /proc/sched_debug | grep dl_bw
```

---

## 10. Performance Considerations

- **PELT (Per-Entity Load Tracking):** Each `sched_entity` tracks its own load average with a decay function. This per-entity load is used by the load balancer to make fine-grained migration decisions.
- **Topology-aware domain hierarchy:** Balance at SMT level first (cheapest), then MC level (LLC shared), then NUMA (most expensive). This minimizes cross-NUMA traffic.
- **`sched_util_clamp`:** Frequency scaling integration — tasks with high utilization request higher CPU frequency via `cpufreq_sugov`. Critical for GPU submission threads that need fast CPU bursts.
- **Idle injection (CPU C-states):** When load balancer leaves a CPU idle, the CPU enters deep C-state. Re-waking from C6 takes ~100 µs — the newidle balancer must be fast enough to pull work before the CPU powers down.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. vruntime formula and why it enables weighted fairness without discrete priority arrays.
2. Scheduling domain hierarchy — SMT → MC → DIE → NUMA — and balance interval scaling.
3. `newidle_balance` as the critical latency path vs periodic `load_balance`.
4. Wake affinity heuristic — waker vs wakee CPU selection.
5. `SCHED_DEADLINE` EDF with admission control — for GPU submission latency guarantees.
6. PELT for fine-grained per-task load tracking (replaces old CPU load averages).
7. NUMA balancing: autonuma vs explicit `mbind()` — and when each applies.
