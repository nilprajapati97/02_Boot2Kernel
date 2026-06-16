# Incident Response Interview Q&A Master Sheet

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

This file consolidates the highest-frequency production memory incident questions from NVIDIA, ARM, Qualcomm, and AMD interview rounds.

Use as a final-review sheet combining signal interpretation, mitigation reasoning, and architectural depth.

---

## 2. ARM64 Hardware Detail

### 2.1 Architectural facts most commonly tested in incident scenarios

- TLB invalidation scope and DSB/ISB requirements
- NUMA latency asymmetry impact on tail latency
- Zone/watermark interaction with NUMA topology
- Cache hierarchy residency effects on reclaim cold-start

### 2.2 Common architectural traps in incident framing

- treating ARM64 identically to x86 in memory ordering — incorrect
- assuming hardware NUMA balancing; ARM does not have HW migrators
- forgetting that TLB shootdowns are software-initiated on ARM64

---

## 3. Linux Kernel Implementation

High-value code and data paths to know:

| Path | Relevance |
|---|---|
| `mm/vmscan.c:shrink_page_list()` | core reclaim logic |
| `mm/oom_kill.c:select_bad_process()` | OOM victim selection |
| `kernel/sched/numa.c` | NUMA balancing decisions |
| `mm/compaction.c:compact_zone()` | compaction entry point |
| `mm/zswap.c` | zswap pool and compression |
| `/proc/pressure/memory` | PSI interface |
| `mm/mglru.c` | multi-gen LRU logic |

---

## 4. Hardware-Software Interaction

The universal production incident model:

1. **hardware pressure point** (TLB churn, cache eviction, remote access)
2. **software amplifier** (misconfigured limits, wrong reclaim policy, NUMA imbalance)
3. **user-visible effect** (p99 latency, error rate, OOM)
4. **mitigation** (reversible cgroup/policy changes first)
5. **fix** (durable policy, automation, capacity/NUMA alignment)

---

## 5. Interview Q and A — Full Master Set

Q1: You receive an alert: p99 memory allocation latency doubled. Where do you start?  
Check PSI memory full trend over 1m/10m windows, then vmstat deltas for `pgscan_direct` and `allocstall`.

Q2: PSI is high but free memory shows 10% available. Explain.  
Reclaim is active but inefficient. Working set overflows tier, causing stall even with some free pages.

Q3: How do cgroup v2 `memory.low` and `memory.high` differ operationally?  
`memory.low` protects a guaranteed minimum; `memory.high` throttles when usage exceeds a soft limit.

Q4: A service is OOM-killed repeatedly. What is the structured fix?  
Set `memory.max` appropriately, tune `oom_score_adj`, protect dependencies with `memory.low`, fix the leak or oversizing.

Q5: kswapd CPU usage is high but latency remains bad. Why?  
`pgscan` >> `pgsteal`: scanning but not reclaiming effectively. Hot working set is too large for available tier.

Q6: How does THP misconfiguration cause an incident?  
`always` mode triggers continuous compaction; split/merge churn raises TLB shootdown rate and inflates IPI load.

Q7: What is the safe order of mitigations during an active memory incident?  
1. throttle non-critical cgroups. 2. protect critical with `memory.low`. 3. reduce allocation burst. 4. investigate root cause offline.

Q8: A NUMA system shows one node at 90% utilization, another at 30%. What is happening?  
NUMA imbalance — either auto-balancing is disabled, affinity is misconfigured, or allocation policies are not NUMA-aware.

Q9: What distinguishes a TLB shootdown storm from a reclaim storm in metrics?  
Shootdown storm correlates with IPI rate and mapping mutation; reclaim storm correlates with `pgscan`/`pgsteal` counters and PSI.

Q10: How do you confirm an incident is fully resolved?  
PSI stable below SLO threshold, p99 at baseline, no emergency limits active, root cause confirmed and fix deployed.

Q11: What is MGLRU and why does it help in storms?  
Multi-gen LRU tracks page access recency across generations; it evicts cold pages more accurately, reducing wasted scans and improving steal efficiency.

Q12: An ARM64 server has frequent `thp_fault_fallback` events. What does this mean?  
2 MB physically contiguous pages cannot be allocated — fragmentation prevents THP. Enable proactive compaction.

---

## 6. Pitfalls and Gotchas

- Memorizing tool outputs without understanding the underlying kernel path.
- Proposing aggressive global kernel parameter changes as first mitigations.
- Confusing `oom_score` (kernel-computed) with `oom_score_adj` (operator-set).
- Forgetting that PSI measures time, not bytes.

---

## 7. Quick Reference Table

| Incident Type | Primary Signal | Safe First Mitigation | Confirms Resolution |
|---|---|---|---|
| allocation stall | PSI full rising | throttle/protect cgroups | PSI and p99 stable |
| reclaim storm | pgscan >> pgsteal | watermark tuning | steal efficiency normalizes |
| NUMA imbalance | remote access ratio | affinity alignment | local access ratio improves |
| THP fragmentation | `thp_fault_fallback` | proactive compaction | high-order buddy lists refill |
| OOM cascade | `oom_kill` events | oom_score_adj + memory.max | zero kills over load window |
| TLB IPI storm | IPI spikes + p99 drift | reduce mapping churn | IPI rate drops with p99 |
