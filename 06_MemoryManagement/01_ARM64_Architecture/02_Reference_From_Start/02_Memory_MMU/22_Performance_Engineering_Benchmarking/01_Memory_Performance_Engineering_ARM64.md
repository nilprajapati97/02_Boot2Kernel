# Memory Performance Engineering on ARM64

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Memory performance engineering is the discipline of measuring, modeling, and optimizing memory-related latency and bandwidth for production workloads.

Core principles:
- measure before tuning
- distinguish bandwidth-bound from latency-bound workloads
- treat tail latency as a first-class metric
- validate every change with repeatable benchmarks

---

## 2. ARM64 Hardware Detail

### 2.1 Performance-relevant hardware features

- multi-level cache hierarchy (L1/L2/L3 with varying inclusivity)
- coherent interconnect (CMN-600, CMN-700, or vendor-specific)
- per-CPU TLBs with shared L2 TLB
- hardware prefetchers (configurable on some vendors)
- ARMv8.4+ MPAM for cache/bandwidth partitioning

### 2.2 Performance signature dimensions

For any workload, characterize:
- working set size relative to cache levels
- access stride and spatial locality
- temporal reuse distance
- read/write ratio
- thread concurrency and false sharing potential

---

## 3. Linux Kernel Implementation

Kernel facilities supporting performance work:
- `perf` subsystem with ARM PMU integration
- BPF for low-overhead live profiling
- `numactl` and `taskset` for placement control
- transparent huge pages (THP) and hugetlb policies
- MGLRU for improved reclaim decision quality

### 3.1 Measurement methodology

1. baseline with steady-state workload and warmed caches
2. record multiple runs, report median and tail (p99, p999)
3. isolate variables — change one tunable at a time
4. include negative controls (verify no improvement when none expected)
5. capture environmental state (kernel version, governor, THP setting)

### 3.2 Common pitfalls in measurement

- not warming caches before measurement
- running on CPU with frequency scaling enabled
- ignoring NUMA placement during benchmark
- single-run results without variance analysis

---

## 4. Hardware-Software Interaction

Effective performance work couples hardware understanding with kernel policy:
- hardware sets ceilings (bandwidth, latency, cache size)
- kernel policy determines how close workloads get to those ceilings
- bad policy can leave 50% of hardware capability unused
- good policy combined with poor code structure also wastes hardware

---

## 5. Interview Q and A

Q1: What is the first question to ask about a memory-bound workload?  
Is it bandwidth-bound or latency-bound? They need different optimization strategies.

Q2: Why measure tail latency rather than average?  
Tail latency reflects user experience and reveals contention that averages hide.

Q3: What is the most common measurement mistake?  
Single runs without variance analysis on systems with frequency scaling.

Q4: How does NUMA placement affect benchmark validity?  
Remote memory access can double or triple effective latency, invalidating cross-system comparisons.

Q5: When are THP gains real vs measurement artifacts?  
Real when working set is large enough to stress TLB; artifact when benchmark fits in L1/L2.

Q6: What is the role of MPAM in performance engineering?  
It enables hardware-enforced cache and bandwidth partitioning, isolating critical workloads.

---

## 6. Pitfalls and Gotchas

- Optimizing micro-benchmarks that do not represent production access patterns.
- Reporting throughput improvements while tail latency regresses.
- Ignoring kernel governor and CPU frequency state during measurement.
- Tuning without baselining first.

---

## 7. Quick Reference Table

| Workload Type | Bottleneck | Primary Optimization |
|---|---|---|
| streaming (large arrays) | bandwidth | prefetch, NT stores, parallelism |
| pointer-chasing | latency | locality, prefetch, data layout |
| transactional | tail latency | placement, isolation, MPAM |
| analytics | mixed | NUMA awareness + cache blocking |
