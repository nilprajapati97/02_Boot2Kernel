# Memory Leak Hunting in Live Systems

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

A leak incident is sustained growth in unreclaimed memory that does not return after workload normalization.

Leak triage goals:
- determine kernel vs userspace source
- quantify growth slope and blast radius
- mitigate safely before deep root-cause work

---

## 2. ARM64 Hardware Detail

### 2.1 Architecture impact

ARM64 itself does not "cause" leaks, but leak side effects surface as:
- higher miss and walk pressure
- reclaim overhead growth
- increased probability of allocator stalls

### 2.2 Observability implications

On high core-count ARM64 systems, background leak growth can stay hidden in averages; cgroup-local signals and per-service telemetry are required.

---

## 3. Linux Kernel Implementation

Primary tooling and paths:
- userspace RSS/PSS tracking by cgroup and process
- slab growth diagnostics (`/proc/slabinfo`, kmem stats)
- kmemleak (where enabled), BPF/perf for allocation hotspots
- memory cgroup protections and limits for containment

### 3.1 Triage sequence

1. verify monotonic memory growth over stable demand
2. split userspace vs kernel growth
3. identify top growing allocators/caches
4. apply containment limits and canary restarts where safe

### 3.2 Containment-first actions

- tighten `memory.high` on suspect non-critical services
- preserve critical services with `memory.low`
- use restart budgets and staged rollouts for suspected binaries

---

## 4. Hardware-Software Interaction

Leak growth raises memory-path contention. As pressure rises, ARM64 translation/cache resources spend more time servicing churn, and user-visible latency deteriorates before OOM.

---

## 5. Interview Q and A

Q1: How do you prove it is a leak and not cache warming?  
Show monotonic growth beyond workload/traffic normalization windows.

Q2: Why separate kernel and userspace early?  
It avoids wasting time on the wrong telemetry stack.

Q3: What is a safe mitigation during investigation?  
Contain suspect workloads with cgroup throttles while protecting critical ones.

Q4: Why can leaks hurt latency before OOM?  
Reclaim and translation churn increase stall time under pressure.

Q5: What is a common false positive?  
Filesystem cache growth that later stabilizes naturally.

Q6: What marks incident closure?  
Growth slope returns to near zero and SLOs remain stable without emergency limits.

---

## 6. Pitfalls and Gotchas

- Equating high RSS with leak without time-series evidence.
- Ignoring kernel slab growth when userspace appears stable.
- Immediate hard kills of critical services without guardrails.
- Ending investigation after mitigation without root-cause fix.

---

## 7. Quick Reference Table

| Signal | Suggests | Immediate Response |
|---|---|---|
| monotonic cgroup usage growth | userspace leak candidate | isolate and cap offender |
| slab cache runaway | kernel leak/caching issue | inspect slab contributors |
| rising PSI + memory growth | pressure-driven latency risk | protect critical workloads |
| repeated OOM in same tenant | unresolved leak path | enforce containment + rollout fix |
