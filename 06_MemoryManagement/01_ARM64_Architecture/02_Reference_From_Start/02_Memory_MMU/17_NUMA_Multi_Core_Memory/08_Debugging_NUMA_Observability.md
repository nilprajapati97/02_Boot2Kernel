# Debugging and Observability of NUMA Behavior Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), tools and instrumentation

---

## 1. Concept Foundation

NUMA issues are notoriously hard to debug without proper observability.

Key challenges:
- latency effects are subtle and workload-dependent
- topology varies widely across platforms
- isolation between tasks makes root cause analysis difficult

---

## 2. ARM64 Hardware Detail

### 2.1 Performance counter support for NUMA

ARM PMU capabilities:
- memory access events (if supported by core)
- cache misses and memory stalls
- bus traffic analysis

Architecture-dependent; not all ARM cores expose full NUMA metrics.

### 2.2 Event tracing for NUMA

ARM can use standard Linux perf and ftrace.
Events of interest:
- page migration
- NUMA faults
- cross-node traffic

---

## 3. Linux Kernel Implementation

### 3.1 /proc interface

Key files:
- /proc/zoneinfo: per-zone free and allocator stats
- /proc/[pid]/numa_maps: task memory distribution
- /proc/meminfo: system-level memory overview

### 3.2 sysfs interface

Key paths:
- /sys/devices/system/node/node*/meminfo: per-node memory
- /sys/kernel/mm/numa_balancing/: Auto-NUMA controls
- /sys/devices/system/node/node*/cpulist: per-node CPU list

### 3.3 numastat tool

High-level NUMA statistics:
- numa_hit: allocation on preferred node
- numa_miss: allocation on non-preferred
- local_node: access from local CPU

### 3.4 perf and ftrace tools

perf commands for NUMA:
```
perf stat -e dTLB-load-misses,dTLB-store-misses ...
perf record -e kmem:mm_migrate_pages ...
perf report
```

ftrace filters:
```
echo mm_migrate_pages > /sys/kernel/debug/tracing/set_event
```

---

## 4. Hardware-Software Interaction

Debugging example:
1. application has poor performance
2. use numastat to check allocation patterns
3. use perf to measure TLB misses and cross-node access
4. check /proc/[pid]/numa_maps for memory placement
5. enable Auto-NUMA tracing to see migration activity
6. correlate with application behavior

---

## 5. Interview Q and A

Q1: How do you start debugging NUMA issues?
Check numastat and /proc/[pid]/numa_maps to understand actual memory distribution.

Q2: What does numa_miss in numastat indicate?
Memory allocated on non-preferred node; high rate suggests allocation pressure or misplaced memory.

Q3: Can you trace page migration events?
Yes, via ftrace or perf on mm_migrate_pages event.

Q4: How do you measure cross-socket traffic?
Via perf memory events, bandwidth counters on some cores, or proxy metrics like TLB misses.

Q5: What is the benefit of enabling NUMA debug output?
Verbose NUMA balancing logs help understand Auto-NUMA decisions (but impacts performance).

Q6: How do you validate NUMA improvements?
Baseline: measure before and after with numastat, perf, and latency/throughput metrics.

---

## 6. Pitfalls and Gotchas

- Over-relying on counters without understanding their meaning on ARM.
- Profiling with NUMA-sensitive workload on non-representative hardware.
- Missing interaction between auto-NUMA and explicit cgroup binding.
- Assuming all NUMA metrics are universally available on all ARM platforms.
- Not accounting for workload phase changes when evaluating NUMA optimization.

---

## 7. Quick Reference Table

| Tool | Primary use |
|---|---|
| numastat | high-level NUMA allocation and access counts |
| /proc/*/numa_maps | per-task memory distribution and placement |
| perf | detailed event counting and profiling |
| ftrace | kernel event tracing and analysis |

| Metric | Interpretation |
|---|---|
| numa_hit high | most allocations going to preferred node (good) |
| numa_miss high | allocation pressure on preferred node (investigate) |
| local_node high | cross-node accesses (latency concern) |
| TLB misses high | potential memory access fragmentation |
