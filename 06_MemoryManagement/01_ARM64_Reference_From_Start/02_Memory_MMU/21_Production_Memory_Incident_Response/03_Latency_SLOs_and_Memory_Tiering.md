# Latency SLOs and Memory Tiering

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Memory tiering places data on tiers (for example local DRAM, remote DRAM, CXL-backed memory) based on cost and latency.

SLO-aware tiering principle:
- critical hot data stays on the fastest tier
- cold or burst-tolerant data can migrate to slower tiers
- migration policy must prioritize tail-latency stability over average throughput

---

## 2. ARM64 Hardware Detail

### 2.1 Tiering-sensitive latency paths

ARM64 latency is highly sensitive to:
- remote access hop count
- interconnect contention
- page migration side effects (TLB invalidation and cache disruption)

### 2.2 Implications for policy design

Policies should consider:
- NUMA locality first
- migration frequency caps
- per-workload fault budget

---

## 3. Linux Kernel Implementation

Relevant mechanisms:
- NUMA balancing and memory migration
- demotion paths and memory tiering policy
- cgroup isolation to avoid cross-tenant interference
- reclaim strategy integration with tier placement

### 3.1 SLO-oriented control loop

1. detect SLO drift (p99, p999)
2. correlate with memory pressure and migration activity
3. pin or protect hot working sets
4. demote cold pages conservatively
5. re-evaluate every short interval with rollback guard

### 3.2 Practical guardrails

- cap migrations per second
- avoid simultaneous reclaim and aggressive demotion
- reserve headroom for foreground services

---

## 4. Hardware-Software Interaction

Tiering errors often create feedback loops:
- migration raises TLB/cache disruption
- disruption increases faults and stalls
- policy overreacts and migrates more

Stable systems combine hardware locality awareness and software hysteresis.

---

## 5. Interview Q and A

Q1: What is the primary tiering objective in production?  
Preserve latency SLOs while reducing memory cost.

Q2: Why can aggressive migration hurt performance?  
It introduces translation/cache churn and remote-access bursts.

Q3: Which metric should gate migration?  
Tail-latency and pressure trends, not only free capacity.

Q4: How does cgroup policy help?  
It prevents one workload's migration strategy from harming others.

Q5: Why keep rollback guardrails?  
Tiering decisions can be wrong under changing traffic shape.

Q6: What indicates success?  
Lower p99 with stable PSI and controlled migration churn.

---

## 6. Pitfalls and Gotchas

- Optimizing for average latency while p99 worsens.
- Demoting pages that are cold only for a short window.
- Ignoring migration overhead in the cost model.
- Running multiple autonomous tuners without coordination.

---

## 7. Quick Reference Table

| Control | Benefit | Risk if Misused |
|---|---|---|
| migration rate cap | avoids churn spikes | under-reacting to real pressure |
| hot-set protection | keeps p99 stable | can starve background work |
| cgroup isolation | tenant fairness | incorrect limits reduce utilization |
| demotion hysteresis | prevents oscillation | slower adaptation to demand shifts |
