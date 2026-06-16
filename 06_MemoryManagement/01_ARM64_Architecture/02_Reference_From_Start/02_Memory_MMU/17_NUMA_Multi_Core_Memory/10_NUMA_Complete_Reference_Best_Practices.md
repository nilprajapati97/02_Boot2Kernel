# NUMA System Complete Reference and Best Practices Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), comprehensive NUMA reference

---

## 1. Concept Foundation

NUMA is a fundamental constraint on modern multi-socket systems.

Mastering NUMA requires:
- understanding topology and latency
- designing allocation and scheduling strategies
- proper observability and debugging skills
- realistic performance expectation setting

---

## 2. ARM64 Hardware Detail Summary

### 2.1 Typical server topology

2-4 sockets, 64-512 cores per socket, proportional DRAM.
Latency differences across sockets observable with proper instrumentation.

### 2.2 Firmware support

ACPI SRAT/SLIT essential for Linux NUMA.
ARM servers rely on platform firmware for topology reporting.

---

## 3. Linux Kernel Implementation Summary

Stack of components:
1. memblock per-node tracking
2. zones per node
3. allocator node affinity
4. scheduler NUMA awareness
5. Auto-NUMA probing and migration
6. kswapd per-node daemons
7. cgroup node constraints
8. page tiering (if configured)

---

## 4. Hardware-Software Interaction: Complete Picture

Lifecycle of NUMA-aware system:
1. boot discovers topology
2. memory allocated to initial tasks somewhat arbitrarily
3. Auto-NUMA samples patterns
4. page migration and scheduler drift tasks toward local memory
5. steady state: most activity is local
6. performance stabilizes around topology constraints

Optimization layers (in order of effort):
1. topology-aware application design (hardest, best payoff)
2. cgroup binding (easier, strong isolation benefit)
3. tuning allocator and scheduler defaults (moderate, per-case tuning)
4. Auto-NUMA configuration (easier, variable benefit)
5. observability and monitoring (always worthwhile)

---

## 5. Interview Q and A

Q1: When is NUMA optimization most critical?
Large-scale HPC, financial trading, database workloads where latency variance is unacceptable.

Q2: Can you turn off NUMA and pretend it's flat memory?
Yes (numa=off), but lose performance; not recommended for multi-socket systems.

Q3: What is the biggest NUMA mistake most engineers make?
Ignoring it on smaller systems or assuming automatic optimizations are always sufficient.

Q4: How do you validate NUMA in your product?
Test on representative 2+ socket hardware with realistic workloads; measure latency and throughput.

Q5: When should applications explicitly manage NUMA?
High-performance compute, databases, and workloads with irregular memory patterns.

Q6: What is the future of NUMA in ARM64?
Likely more sockets, heterogeneous memory tiers, and finer-grained placement controls.

---

## 6. Pitfalls and Gotchas

- Premature NUMA tuning without profiling (most workloads don't care).
- Blindly copying topology-aware code from other projects.
- Confusing CPU affinity with NUMA placement (different concerns).
- Over-tuning Auto-NUMA aggressiveness and causing page thrashing.
- Not accounting for shared memory in multi-process workloads.
- Assuming heterogeneous NUMA (different memory speeds) always requires explicit application handling.

---

## 7. Quick Reference: Full NUMA Stack

| Layer | Tool/File | Purpose |
|---|---|---|
| Topology discovery | ACPI SRAT/SLIT | define node layout and latency |
| Memory organization | zones per node | allocator organization |
| Allocation | GFP flags + allocator | prefer local node |
| Scheduling | scheduler NUMA hooks | place task near memory |
| Background optimization | Auto-NUMA + kswapd | maintain locality over time |
| Explicit control | cgroups (cpuset.mems) | enforce isolation |
| Observability | numastat, perf, ftrace | measure and debug |

| Decision | Typical logic |
|---|---|
| memory allocation | prefer local node, fallback to remote |
| task wake placement | prefer node with task's memory |
| page migration | move page toward local node if benefit clear |
| reclaim trigger | per-node watermarks independent |
| cgroup binding | hard constraint overrides defaults |
