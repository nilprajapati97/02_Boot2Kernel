# Performance Engineering Complete Reference

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

This file consolidates the performance engineering workflow for ARM64 memory systems into a single reference.

The disciplined cycle:
1. characterize workload memory behavior
2. measure with reproducible methodology
3. identify dominant bottleneck (bandwidth, latency, contention)
4. apply targeted optimization (code, layout, kernel policy, hardware partitioning)
5. validate with end-to-end metrics
6. document for future regression detection

---

## 2. ARM64 Hardware Detail

### 2.1 Performance ceilings to know

| Resource | Typical Ceiling (modern server core) |
|---|---|
| L1D bandwidth | 80-100 GB/s per core |
| L2 bandwidth | 40-60 GB/s per core |
| LLC bandwidth | 20-40 GB/s per core |
| DRAM bandwidth (per socket) | 200-400 GB/s (multi-channel) |
| L1D latency | 1-2 ns |
| LLC latency | 10-17 ns |
| DRAM latency (local) | 67-100 ns |
| DRAM latency (remote NUMA) | 133-200 ns |

### 2.2 ARM-specific performance levers

- LSE atomics (ARMv8.1) — better contention behavior
- THP and hugetlb — reduce TLB walks
- MPAM (ARMv8.4) — hardware partitioning
- prefetcher tuning (where exposed)
- big.LITTLE scheduling considerations on heterogeneous SoCs

---

## 3. Linux Kernel Implementation

### 3.1 Key tools

| Tool | Primary Use |
|---|---|
| `perf stat` | counter aggregation |
| `perf record/report` | sampling profiler with PMU events |
| `perf c2c` | cache coherence/false sharing analysis |
| `perf mem` | memory access latency profile |
| `perf lock` | kernel lock contention |
| `bpftrace` / `bcc` | custom dynamic tracing |
| `numactl` / `taskset` | placement control |
| STREAM, lmbench | bandwidth/latency microbenchmarks |
| `resctrl` interface | MPAM partitioning |

### 3.2 Kernel tunables for performance

| Tunable | Purpose |
|---|---|
| `/sys/kernel/mm/transparent_hugepage/enabled` | THP policy |
| `/proc/sys/vm/swappiness` | reclaim preference |
| `/proc/sys/vm/zone_reclaim_mode` | NUMA reclaim behavior |
| `/sys/kernel/mm/lru_gen/enabled` | MGLRU activation |
| `/proc/sys/kernel/numa_balancing` | auto NUMA migration |
| `cpupower frequency-set` | governor and frequency |

---

## 4. Hardware-Software Interaction

Memory performance is determined by the alignment of:
- **workload characteristics** — access pattern, footprint, concurrency
- **code structure** — data layout, algorithm, synchronization
- **kernel policy** — placement, reclaim, scheduling
- **hardware capabilities** — caches, interconnect, partitioning

Excellence in any one layer is wasted if another layer mismatches.

---

## 5. Interview Q and A — Performance Engineering Master Set

Q1: How do you determine if a workload is bandwidth-bound or latency-bound?  
Bandwidth-bound: performance scales with thread count until DRAM saturated, low IPC. Latency-bound: limited by individual access serialization, low memory bandwidth utilization, high stalls per access.

Q2: A workload runs at 50% of expected throughput. Walk through diagnosis.  
1. baseline (CPU usage, IPC, memory bandwidth via perf). 2. cache miss rates at each level. 3. TLB miss rate. 4. NUMA local/remote ratio. 5. lock contention. 6. compare against microbenchmark of similar pattern. 7. attribute to specific code/data with sampling profiler.

Q3: How does THP impact performance positively and negatively?  
Positive: reduces TLB pressure for large working sets. Negative: compaction overhead, split/merge churn under fragmentation, potential memory waste if only small region is hot.

Q4: When does NUMA-aware allocation matter most?  
For workloads where memory access dominates over compute and working set is larger than per-node LLC. Less critical for compute-bound or small-footprint code.

Q5: How would you optimize a hot per-CPU counter?  
Replace shared atomic with per-CPU variable, aggregate on read. If aggregation is too expensive, use percpu_counter API which batches updates.

Q6: What is the typical performance impact of false sharing?  
2-10x slowdown on contended fields, scales worse with core count. Detected via `perf c2c`; fixed with cache-line alignment.

Q7: How does MPAM differ from cgroup CPU/memory limits?  
cgroups limit at software boundaries (scheduler, allocator); MPAM enforces at hardware (cache, memory controller) on every access.

Q8: A change improves throughput but regresses p99. How do you reason about it?  
The change likely benefits average-case at cost of worst-case behavior (e.g., more aggressive batching, weaker isolation). For latency-critical services, prefer the slower-throughput option.

Q9: How do you ensure benchmark results are reproducible?  
Fix CPU frequency (performance governor), pin to specific cores and NUMA node, disable THP/idle states for measurement, run multiple iterations, report median and variance.

Q10: What is the role of MGLRU in performance?  
Better eviction decision quality reduces refault rate and reclaim CPU overhead, improving steady-state throughput under memory pressure.

Q11: How does ARM's weakly-ordered memory model affect performance code?  
Allows more reordering and parallelism than x86 TSO; requires careful barrier placement for correctness but enables higher throughput when used correctly. Acquire/release atomics encode barriers efficiently.

Q12: When should you stop optimizing?  
When the workload meets SLO with margin, further changes risk maintainability, or remaining headroom is below measurement noise.

---

## 6. Pitfalls and Gotchas

- Reporting any benchmark result without environment details (kernel, governor, NUMA setup).
- Optimizing for throughput while regressing tail latency.
- Adding complexity to code for theoretical wins without measurement.
- Ignoring that hardware partitioning (MPAM) reduces total capacity for non-partitioned workloads.
- Confusing peak achievable bandwidth (microbenchmark) with sustained production bandwidth.

---

## 7. Quick Reference Table

| Bottleneck Type | Diagnostic Signal | Primary Optimization |
|---|---|---|
| DRAM bandwidth | high memory traffic, low IPC | reduce footprint, improve locality |
| LLC capacity | high LL_CACHE_MISS rate | cache blocking, working set reduction |
| TLB | high DTLB_WALK rate | THP, hugetlb, data layout |
| NUMA | high remote access ratio | placement, affinity, interleave |
| false sharing | high HITM events | align/pad to cache line |
| lock contention | high lock wait time | RCU, per-CPU, lock striping |
| noisy neighbor | LLC/bandwidth pollution | MPAM partitioning |
| reclaim | high `pgscan` | adjust watermarks, enable MGLRU |
