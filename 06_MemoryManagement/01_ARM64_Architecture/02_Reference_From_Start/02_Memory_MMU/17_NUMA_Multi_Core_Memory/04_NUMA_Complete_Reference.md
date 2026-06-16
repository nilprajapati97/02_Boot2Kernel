# ARM64 NUMA Complete Reference

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), multi-socket and multi-node systems

---

## 1. Concept Foundation

NUMA systems require coordinated memory and task placement to minimize remote-access latency.

Stack of mechanisms:
- hardware topology discovery and distance reporting
- memory allocator affinity
- CPU scheduler NUMA awareness
- background Auto-NUMA balancing
- explicit cgroup constraints

Goal:
- maximize local memory access
- reduce cross-socket latency effects
- improve overall system throughput and latency

---

## 2. ARM64 Hardware Detail

### 2.1 Multi-socket architecture overview

Typical 2-socket ARM64 server:
- socket 0: 64 cores, 512GB DRAM (local)
- socket 1: 64 cores, 512GB DRAM (local)
- CMN (Coherent Mesh Network) intra-socket
- proprietary die-to-die or PCIe cross-socket link

### 2.2 Access latency model

Local: ~70ns  
Remote: ~140-200ns (2-3× multiplier common)  
Impact: every 50ns latency difference affects many workloads measurably

### 2.3 Topology discovery

ACPI SRAT and SLIT tables define:
- which CPUs are on which nodes
- which memory ranges belong to which nodes
- relative latency between all node pairs

---

## 3. Linux Kernel Implementation Summary

Core path:
1. ACPI table parsing → node topology
2. memblock per-node accounting
3. zones created per node and type
4. allocator defaults to local node
5. scheduler tracks per-task NUMA metrics
6. Auto-NUMA probes and migrates
7. cgroup policies can override or constrain

---

## 4. Hardware-Software Interaction

Lifecycle:
1. boot discovers topology
2. memory is carved into zones per node
3. tasks start and initially run somewhat randomly
4. Auto-NUMA samples remote access patterns
5. page migration moves frequently-accessed pages to local node
6. scheduler prefers task placement near updated local memory
7. steady state: most access is local

Operational benefit:
- application sees memory locality improve automatically
- latency variance often decreases over time

---

## 5. Interview Q and A

Q1: How is NUMA different from just having multiple memory banks?
NUMA makes latency observable and requires explicit optimization; flat memory model hides it.

Q2: What does /proc/zoneinfo tell about NUMA?
Per-zone free pages and allocator statistics; distinct zones per node show NUMA structure.

Q3: Why is ACPI SLIT important?
It defines the latency matrix so scheduler and allocator can make informed placement decisions.

Q4: Can you disable NUMA optimizations and what happens?
Yes (numa=off kernel parameter). System treats all memory as local access cost, losing optimization.

Q5: How do you validate NUMA improvements?
Measure latency variance and throughput; use numastat or custom instrumentation to verify locality.

Q6: What is the biggest complexity NUMA introduces?
The coupling of memory, scheduling, and migration decisions across multiple subsystems.

---

## 6. Pitfalls and Gotchas

- Assuming NUMA optimization is automatic without application cooperation.
- Ignoring cgroup constraints when analyzing placement.
- Misinterpreting SLIT distances (relative, not absolute latency).
- Over-tuning Auto-NUMA aggressiveness globally.
- Forgetting that shared memory workloads can't optimize easily.

---

## 7. Quick Reference Summary

| Layer | Key concept |
|---|---|
| Hardware | multi-socket with local memory per socket |
| ACPI/DT | topology and latency matrix |
| Zones | per-node memory organization |
| Allocator | local-first preference |
| Scheduler | task placement near memory |
| Auto-NUMA | background page migration |
| cgroups | explicit node constraints |

| Observable | Indicator |
|---|---|
| /proc/zoneinfo | memory distribution per node |
| /proc/[pid]/numa_maps | task memory distribution |
| numastat | per-node allocation patterns |
| perf | cross-socket memory access frequency |
