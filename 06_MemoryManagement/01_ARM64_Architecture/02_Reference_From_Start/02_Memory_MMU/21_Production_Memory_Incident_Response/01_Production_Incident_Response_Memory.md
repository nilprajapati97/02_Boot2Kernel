# Production Memory Incident Response on ARM64

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Production memory incidents are not only "out of memory" events. They include latency cliffs, reclaim storms, NUMA imbalance, TLB shootdown amplification, and swap feedback loops.

A practical response model:
- classify symptom (latency, throughput, crash, or correctness)
- identify pressure domain (allocator, reclaim, translation, cache, I/O)
- apply reversible mitigations first
- confirm recovery with service-level metrics

---

## 2. ARM64 Hardware Detail

### 2.1 Translation and hierarchy behavior in incidents

ARM64 latency incidents frequently correlate with:
- page-table walk amplification after TLB churn
- contention in coherent interconnect and last-level cache
- NUMA remote accesses on multi-socket or multi-die systems

### 2.2 Architectural controls that influence blast radius

Useful controls and properties:
- ASID/TLB behavior affects context-switch and shootdown cost
- barrier-heavy code paths can serialize critical sections under pressure
- memory attribute mismatches can magnify device and DMA stalls

---

## 3. Linux Kernel Implementation

Incident handling anchors in these subsystems:
- `mm/vmscan.c` and reclaim pressure accounting
- PSI (`/proc/pressure/memory`) for sustained-stall visibility
- cgroup v2 memory controllers (`memory.high`, `memory.max`, `memory.low`)
- THP policy (`always`, `madvise`, `never`) and compaction side effects

### 3.1 Fast triage signal set

Collect immediately:
- PSI memory `some` and `full`
- vmstat counters (`pgscan`, `pgsteal`, `allocstall`, `kswapd` activity)
- cgroup event counters and OOM logs
- service p99/p999 latency and error rate

### 3.2 Safe-first mitigation ladder

1. reduce allocation burst rate (throttle non-critical work)
2. protect critical cgroups with `memory.low`
3. constrain noisy neighbors with `memory.high`
4. tune THP policy if compaction dominates
5. apply swap/zswap tuning as temporary pressure valve

---

## 4. Hardware-Software Interaction

Hardware gives translation, caching, and coherence primitives; Linux policy determines who gets memory first during contention.

When policies mismatch workload behavior, the hardware amplifies pain:
- more misses lead to more walks
- more walks increase shared fabric pressure
- tail latency rises before utilization appears saturated

---

## 5. Interview Q and A

Q1: What is the first metric you trust in memory incidents?  
PSI memory trends, because they capture actual stalled time, not just occupancy.

Q2: Why can free memory look healthy while latency is bad?  
Reclaim/compaction and translation churn can stall allocators and threads before total memory is exhausted.

Q3: How does cgroup v2 help incident containment?  
It provides workload-scoped throttling and protection boundaries.

Q4: Why are reversible mitigations important?  
Incidents are noisy; reversible steps reduce risk of secondary regressions.

Q5: What confirms mitigation success?  
Sustained improvement in PSI and tail latency, not only a temporary drop in scans.

Q6: What is a common anti-pattern?  
Blindly disabling swap/THP globally without identifying the real bottleneck.

---

## 6. Pitfalls and Gotchas

- Treating memory incidents as single-metric problems.
- Acting on host-wide counters while cgroup-local pressure is the root cause.
- Ignoring translation overhead and focusing only on allocator stats.
- Applying irreversible tuning changes during active outage.

---

## 7. Quick Reference Table

| Signal | Interpretation | Immediate Action |
|---|---|---|
| PSI memory full rising | direct stall pressure | throttle best-effort allocators |
| `allocstall` spikes | direct reclaim in hot paths | protect critical cgroups |
| `pgscan` >> `pgsteal` | inefficient reclaim | reassess reclaim knobs/working set |
| p99 latency drift + stable CPU | memory path contention | correlate with reclaim + TLB churn |
