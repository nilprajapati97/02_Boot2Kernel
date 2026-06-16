# Interpreting PSI and Reclaim Signals

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Pressure Stall Information (PSI) answers: "How much time did tasks spend waiting on memory?"

Reclaim counters answer: "How hard did the kernel try to recover pages?"

Use both. PSI shows user-visible pain; reclaim counters show kernel effort.

---

## 2. ARM64 Hardware Detail

### 2.1 Why PSI maps well to ARM64 behavior

ARM64 systems can hide contention in average utilization while tail stalls increase due to:
- translation walk pressure
- memory fabric queueing
- cache residency collapse under mixed workloads

### 2.2 Architectural events to correlate

Correlate PSI with:
- TLB miss behavior
- LLC miss rates
- remote NUMA traffic (if applicable)

This distinguishes reclaim pressure from pure compute contention.

---

## 3. Linux Kernel Implementation

Data sources:
- `/proc/pressure/memory` and cgroup-level PSI files
- `/proc/vmstat` (`pgscan_*`, `pgsteal_*`, `workingset_refault`, `allocstall`)
- `memory.events` per cgroup

### 3.1 Read pattern for incidents

1. PSI trend over 1m and 10m windows
2. scan/steal efficiency ratio
3. refault behavior (working set instability)
4. cgroup event deltas (`high`, `max`, `oom`, `oom_kill`)

### 3.2 Decision hints

- High PSI + low steals: reclaim inefficiency or wrong victims
- High refaults: active set does not fit policy/tier
- High `memory.high` events: throttling is active; check fairness

---

## 4. Hardware-Software Interaction

When working sets exceed effective cache and local-memory capacity, ARM64 cores spend more cycles in miss handling and walks. Linux reclaim decisions then determine whether that pressure stabilizes or oscillates.

---

## 5. Interview Q and A

Q1: Why is PSI better than free-memory percentage?  
PSI measures time lost to pressure, which aligns with latency impact.

Q2: What does `pgscan` much greater than `pgsteal` suggest?  
Kernel is scanning many pages but reclaiming little useful memory.

Q3: Why watch `workingset_refault`?  
It reveals churn where reclaimed pages are quickly needed again.

Q4: How do cgroups improve diagnosis?  
They localize pressure to specific workloads instead of host averages.

Q5: What if PSI is high but OOM is absent?  
System is surviving through stalls; latency SLOs may still be violated.

Q6: First safe mitigation?  
Throttle non-critical allocators and protect critical cgroups.

---

## 6. Pitfalls and Gotchas

- Treating one snapshot as truth; trends matter.
- Ignoring cgroup-level PSI while host-level PSI looks moderate.
- Overfitting on single vmstat counters without workload context.
- Confusing reclaimed bytes with meaningful performance recovery.

---

## 7. Quick Reference Table

| Pattern | Meaning | Response |
|---|---|---|
| PSI full rising steadily | sustained allocator stalls | reduce pressure at source |
| high scan, low steal | reclaim inefficiency | adjust reclaim/tiering policy |
| high refault | oscillating working set | protect hot set, reduce churn |
| frequent `memory.high` events | throttle pressure | rebalance limits and guarantees |
