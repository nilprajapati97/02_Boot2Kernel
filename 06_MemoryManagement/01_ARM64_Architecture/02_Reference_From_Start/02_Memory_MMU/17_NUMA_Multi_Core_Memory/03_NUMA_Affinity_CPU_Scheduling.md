# NUMA Node Affinity and CPU Scheduling Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), multi-socket systems

---

## 1. Concept Foundation

NUMA-aware CPU scheduling tries to keep tasks on CPUs whose local NUMA node contains the task's memory.

Rationale:
- local memory access is faster
- remote access crosses interconnect and incurs latency penalty
- scheduler tries to minimize remote accesses

Linux implementation:
- scheduler tracks task NUMA faults and memory locality
- placement decisions influence wake-up and migration

---

## 2. ARM64 Hardware Detail

### 2.1 NUMA node layout discovery

ACPI SRAT provides:
- processor to node mapping via APIC/GIC affinity
- memory to node mapping via address ranges

Linux builds:
- cpu_to_node array
- node_to_cpumask_map
- node memory ranges

### 2.2 Distance model

SLIT provides relative latency matrix.
Affects scheduler ranking in fallback zonelist and allocation preference.

### 2.3 Multi-socket interconnect

Typical server: PCIe mesh, proprietary die-to-die link, or fabric switch.
Access pattern:
- same-socket: latency L
- cross-socket: latency 2-3× L typical

---

## 3. Linux Kernel Implementation

### 3.1 Task NUMA statistics

scheduler tracks per-task NUMA data:
- numa_preferred_nid: best node for this task
- numa_faults[]: fault distribution across nodes
- total_numa_faults: total fault count

### 3.2 Scheduling decision points

When waking a task, scheduler considers:
- where task's memory currently lives
- CPU cache affinity on same socket
- overall system load balance

Tradeoff:
- NUMA placement (memory locality)
- load balancing (CPU utilization fairness)

### 3.3 NUMA balancing and periodic probes

Auto-NUMA periodically marks pages PROT_NONE to probe access locality.
Rationale:
- lightweight sampling of actual memory usage
- cheaper than full perf counter dependency

### 3.4 migrate_pages and final placement

Expensive operation: move page to different node.
Triggered when:
- task accumulates enough remote faults
- benefits significantly outweigh migration cost

---

## 4. Hardware-Software Interaction

Task placement example:
1. task wakes up on socket 0 CPU 0.
2. scheduler checks numa_preferred_nid.
3. if memory is on socket 1, cross-socket access cost is considered.
4. decide: stay on socket 0 for cache benefit or move to socket 1 for memory.
5. eventually, NUMA balancing may trigger page migration.

Long-term behavior:
- task drifts toward socket with most memory
- page migration follows task locality improvement

---

## 5. Interview Q and A

Q1: Why is CPU-to-memory locality often better than cache affinity alone?
Because cross-socket latency penalty (typically 50-100ns additional) often dominates cache miss cost.

Q2: What is numa_preferred_nid?
The scheduler's assessment of which NUMA node has the most of a task's memory.

Q3: How does load balancing constrain NUMA placement?
Overloaded nodes may reject further migration, keeping tasks elsewhere.

Q4: Why probe memory access with PROT_NONE instead of perf counters?
Lower overhead, no dependency on hardware PMU availability or licensing.

Q5: What causes inefficient NUMA placement?
Shared memory workloads where "best" node is ambiguous; periodic large batch operations moving tasks around.

Q6: How do you debug NUMA placement issues?
Inspect /proc/[pid]/numa_maps and task_struct.numa_faults to see actual distribution versus expected.

---

## 6. Pitfalls and Gotchas

- Confusing NUMA affinity with CPU affinity (related but distinct).
- Assuming Linux NUMA scheduling eliminates all remote access (it reduces, not eliminates).
- Over-tuning affinity policy for workloads where it makes little difference.
- Forgetting that page migration itself has cost and overhead.
- Missing interaction between cgroup cpuset constraints and NUMA placement.

---

## 7. Quick Reference Table

| Metric | Typical insight |
|---|---|
| local_faults | task accesses memory on same node |
| remote_faults | task accesses memory across sockets |
| total_numa_faults | overall memory activity rate |
| migration events | page moves between nodes |

| Decision | Typical logic |
|---|---|
| wake placement | prefer node with most task memory |
| load migration | balance CPU utilization if NUMA cost acceptable |
| page migration | move page if remote-fault rate justifies cost |
