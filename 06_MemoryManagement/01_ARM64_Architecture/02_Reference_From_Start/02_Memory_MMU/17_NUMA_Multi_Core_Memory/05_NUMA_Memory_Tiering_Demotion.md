# NUMA-Aware Memory Tiering and Demotion Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), heterogeneous memory tiers

---

## 1. Concept Foundation

Memory tiering extends NUMA with multiple memory types ranked by performance.

Examples:
- DRAM (fast, local)
- persistent memory or other slow tier (capacity-oriented)
- swap or block device (slowest)

Kernel goal:
- keep hot data in fast tier
- transparently demote cold data to slower tier
- improve effective capacity per socket

---

## 2. ARM64 Hardware Detail

### 2.1 Multi-tier memory systems

Typical tiering:
- HBM (high-bandwidth memory) or local DRAM on socket
- persistent memory (PMEM) next to socket
- remote DRAM on different socket
- NVMe swap furthest

### 2.2 Memory tier attributes

Each tier has:
- latency range
- bandwidth capability
- capacity
- cost (power, physical space)

---

## 3. Linux Kernel Implementation

### 3.1 Memory tiers framework

kernel/sched/topology.c and mm/memory-tiers.c manage:
- tier definitions and capabilities
- page demotion policy
- migration between tiers

### 3.2 Page demotion

When memory pressure or migration policy triggers:
1. identify candidate pages
2. allocate space in lower tier
3. migrate page content
4. update page mapping
5. optionally keep metadata in fast tier

### 3.3 Access promotion

Infrequent access to slow tier may warrant promotion back to fast tier.
Policy depends on tier availability and cost.

---

## 4. Hardware-Software Interaction

Workload example:
1. hot working set fits in fast tier
2. cold data resides in slow tier
3. access pattern shifts
4. kernel detects increased slow-tier access
5. promotion brings hot data back to fast tier
6. performance stabilizes

Efficiency:
- more effective capacity than flat DRAM alone
- but adds latency variance and migration overhead

---

## 5. Interview Q and A

Q1: How is memory tiering different from swap?
Tiering explicitly ranks performance tiers and migrates data automatically; swap is typically fallback only.

Q2: What triggers demotion?
Memory pressure, tier fill threshold, or explicit policy on cold data.

Q3: How do you measure tier effectiveness?
Track access latency by tier, promotion/demotion rates, and overall cache hit efficiency.

Q4: Can tiering be transparent to applications?
Mostly yes, though latency variance may become observable.

Q5: Why not just use largest DRAM?
Cost and power constraints often make capacity tiers necessary for large memory systems.

Q6: How does tiering interact with NUMA affinity?
NUMA affinity typically applies first; tiering manages within-node capacity distribution.

---

## 6. Pitfalls and Gotchas

- Assuming tiering is always beneficial (some workloads see latency variance degrade performance).
- Tuning demotion aggressively and causing thrashing.
- Not accounting for migration cost when evaluating benefit.
- Forgetting persistent memory has different reliability and characteristics than DRAM.

---

## 7. Quick Reference Table

| Tier | Typical role |
|---|---|
| HBM/local DRAM | hot working set, latency-sensitive |
| PMEM or remote DRAM | capacity expansion |
| Swap | emergency spill, lowest priority |

| Event | Policy response |
|---|---|
| pressure on fast tier | demote cold pages to slow tier |
| repeated slow-tier access | promote back to fast tier |
| sustained high latency | may inhibit promotion to avoid thrashing |
