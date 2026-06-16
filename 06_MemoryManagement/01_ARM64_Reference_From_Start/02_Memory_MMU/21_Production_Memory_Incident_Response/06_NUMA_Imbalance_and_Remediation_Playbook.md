# NUMA Imbalance and Remediation Playbook

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

NUMA imbalance occurs when a workload frequently accesses remote memory despite local CPU execution.

Effects:
- higher access latency
- bandwidth contention on interconnect links
- elevated tail latency for memory-intensive services

---

## 2. ARM64 Hardware Detail

### 2.1 NUMA characteristics on ARM64 servers

Common deployment realities:
- asymmetric locality between dies/sockets
- varying remote hop penalties
- shared fabric bottlenecks under mixed traffic

### 2.2 Placement-sensitive behavior

Performance is best when CPU affinity, memory policy, and allocator behavior align to keep hot data local.

---

## 3. Linux Kernel Implementation

Kernel levers and observations:
- automatic NUMA balancing and page migration
- CPU pinning/affinity strategies
- cpuset and memory policy controls
- per-node pressure and reclaim counters

### 3.1 Diagnosis checklist

1. quantify local vs remote access ratio
2. verify CPU scheduling locality
3. inspect per-node pressure/reclaim asymmetry
4. identify cross-node allocator bursts

### 3.2 Remediation ladder

- fix CPU affinity drift for critical threads
- apply cpuset boundaries for noisy neighbors
- tune NUMA balancing aggressiveness
- protect node headroom for latency-critical services

---

## 4. Hardware-Software Interaction

NUMA locality is a combined contract:
- hardware provides multiple latency domains
- scheduler and memory policy decide whether accesses stay local

Breaking that contract turns available bandwidth into latency amplification.

---

## 5. Interview Q and A

Q1: Why does remote memory matter for p99?  
Small average penalties become large tails under queueing and contention.

Q2: What is the first remediation in outage mode?  
Restore CPU/memory locality for critical services.

Q3: Can automatic NUMA balancing hurt?  
Yes, if migration churn exceeds locality benefit.

Q4: Which metric validates improvement?  
Remote-access ratio decreases with stable or lower PSI and better p99.

Q5: Why use cpusets/cgroups together?  
They coordinate compute and memory boundaries per workload.

Q6: Common anti-pattern?  
Global policy changes without tenant-level impact analysis.

---

## 6. Pitfalls and Gotchas

- Assuming scheduler locality implies memory locality.
- Over-migrating pages during transient traffic shifts.
- Ignoring per-node reclaim imbalance.
- Pinning too tightly and creating new hotspots.

---

## 7. Quick Reference Table

| Observation | Interpretation | Action |
|---|---|---|
| high remote access ratio | poor locality | align affinity and memory policy |
| one node high reclaim | asymmetric pressure | rebalance placement |
| migration spikes + no SLO gain | churn > benefit | reduce balancing aggressiveness |
| p99 improves after pinning | locality was root cause | codify policy baseline |
