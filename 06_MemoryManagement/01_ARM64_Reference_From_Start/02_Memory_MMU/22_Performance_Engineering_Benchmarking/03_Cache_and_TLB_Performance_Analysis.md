# Cache and TLB Performance Analysis

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Cache and TLB miss rates dominate memory-bound workload performance.

Analysis goals:
- quantify miss rates at each level
- attribute misses to specific code or data structures
- correlate miss rates with end-to-end latency
- identify whether to fix code, layout, or kernel policy

---

## 2. ARM64 Hardware Detail

### 2.1 Cache hierarchy events

Per-level PMU events (typical ARMv8 PMUv3 set):
- `L1D_CACHE` / `L1D_CACHE_REFILL` — L1 data cache access/miss
- `L1I_CACHE` / `L1I_CACHE_REFILL` — L1 instruction
- `L2D_CACHE` / `L2D_CACHE_REFILL` — L2 unified
- `LL_CACHE` / `LL_CACHE_MISS` — last-level cache
- `MEM_ACCESS` — total memory accesses

### 2.2 TLB events

- `L1D_TLB` / `L1D_TLB_REFILL` — data TLB
- `L1I_TLB` / `L1I_TLB_REFILL` — instruction TLB
- `DTLB_WALK` / `ITLB_WALK` — page table walks (TLB miss + walk)
- `L2D_TLB_REFILL` — L2 TLB refills (where L2 TLB exists)

### 2.3 Derived metrics

| Metric | Formula | Healthy Range |
|---|---|---|
| L1D miss rate | L1D_CACHE_REFILL / L1D_CACHE | < 5% |
| LLC miss rate | LL_CACHE_MISS / LL_CACHE | workload-dependent |
| DTLB walk rate | DTLB_WALK / MEM_ACCESS | < 0.1% |
| MPKI | (misses × 1000) / instructions | < 10 typical |

---

## 3. Linux Kernel Implementation

### 3.1 perf invocations for cache/TLB analysis

```bash
# Overall miss rates for a process
perf stat -e L1D_CACHE,L1D_CACHE_REFILL,LL_CACHE,LL_CACHE_MISS \
          -e DTLB_WALK,ITLB_WALK,MEM_ACCESS \
          -p <pid> sleep 30

# Per-symbol cache miss attribution
perf record -e L1D_CACHE_REFILL -- ./workload
perf report --sort symbol

# Cache miss profiling with call stacks
perf record -e LL_CACHE_MISS --call-graph dwarf -- ./workload
perf report --stdio

# TLB walk hot spots
perf record -e DTLB_WALK -- ./workload
perf report
```

### 3.2 Interpretation guide

- high L1D miss + low LLC miss → working set fits in L2/L3; optimize for L1
- high LLC miss → working set exceeds cache; reduce footprint or improve reuse
- high DTLB walk rate → consider THP or hugetlb to expand TLB coverage
- high ITLB walk rate → code footprint problem; consider compiler PGO/layout

### 3.3 Cache blocking example

```c
// Bad: poor cache reuse
for (i = 0; i < N; i++)
    for (j = 0; j < N; j++)
        C[i][j] = A[i][j] + B[i][j];

// Better: blocked for L1 cache reuse
for (ii = 0; ii < N; ii += B)
    for (jj = 0; jj < N; jj += B)
        for (i = ii; i < ii+B; i++)
            for (j = jj; j < jj+B; j++)
                C[i][j] = A[i][j] + B[i][j];
```

Block size B chosen so 3 blocks fit in L1D.

---

## 4. Hardware-Software Interaction

Cache and TLB are precious shared resources. Code that ignores their structure pays multiple times:
- L1 miss costs 10-15 cycles
- LLC miss costs 200-300 cycles
- TLB walk costs 50-100 cycles (4 memory accesses for 4-level table)

A workload with 10% L1D miss rate and 1% TLB walk rate spends as much time waiting as computing.

---

## 5. Interview Q and A

Q1: What does MPKI stand for and why is it useful?  
Misses Per Kilo Instructions — normalizes cache misses across workloads with different instruction counts.

Q2: Why might LLC miss rate be misleading by itself?  
A small absolute number of accesses with 100% miss rate is less concerning than a huge access count with 10% miss rate.

Q3: How does THP reduce DTLB walks?  
Each 2 MB THP entry covers 512x more virtual address space than a 4 KB entry, increasing effective TLB reach.

Q4: What does it mean if `MEM_ACCESS` is much larger than `L1D_CACHE`?  
Many accesses are satisfied by other paths (e.g., store buffer, write combining); investigate counter definitions.

Q5: Why is cache blocking effective?  
It reuses data while still in cache, before eviction by subsequent accesses.

Q6: When is hardware prefetching counterproductive?  
On random access patterns — prefetcher fetches unused cache lines, evicting useful data.

---

## 6. Pitfalls and Gotchas

- Comparing miss rates between workloads with very different instruction counts (use MPKI instead).
- Ignoring instruction-side cache and TLB metrics in code-footprint-heavy workloads.
- Using x86 PMU event names on ARM64 — they differ.
- Treating high miss count as bad without context — high-throughput workloads naturally generate more misses.

---

## 7. Quick Reference Table

| Event | What It Measures | When to Watch |
|---|---|---|
| `L1D_CACHE_REFILL` | L1D miss | hot-path data analysis |
| `LL_CACHE_MISS` | LLC miss | working-set sizing |
| `DTLB_WALK` | TLB miss + walk | large-footprint workloads |
| `ITLB_WALK` | code TLB miss | large code paths |
| `MEM_ACCESS` | total memory ops | normalization base |
