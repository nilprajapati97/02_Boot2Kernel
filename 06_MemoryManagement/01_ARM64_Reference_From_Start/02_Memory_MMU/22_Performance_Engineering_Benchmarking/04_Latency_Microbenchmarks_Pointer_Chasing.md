# Latency Microbenchmarks — lat_mem_rd and Pointer Chasing

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Latency microbenchmarks measure the time for a single memory access at each level of the hierarchy.

They expose:
- L1, L2, L3 hit latencies
- DRAM access latency
- TLB miss penalty
- prefetcher effectiveness

Pointer chasing is the standard technique because it defeats prefetchers and out-of-order execution from hiding latency.

---

## 2. ARM64 Hardware Detail

### 2.1 Typical latencies on modern ARM64 server cores

| Level | Typical Cycles | Typical ns at 3 GHz |
|---|---|---|
| L1D hit | 3-5 | 1-2 |
| L2 hit | 10-15 | 3-5 |
| L3/LLC hit | 30-50 | 10-17 |
| local DRAM | 200-300 | 67-100 |
| remote NUMA | 400-600 | 133-200 |

Exact values vary by SoC and frequency.

### 2.2 Why pointer chasing is needed

Linear access benchmarks are hidden by:
- hardware prefetchers
- out-of-order execution
- store buffers

A random pointer chain forces each load to wait for the previous load's result.

---

## 3. Linux Kernel Implementation

### 3.1 lat_mem_rd from lmbench

```bash
# Build and install lmbench, then:
lat_mem_rd 1024M 64

# Output: stride bytes, latency in ns
# size_MB latency_ns
# 0.00781  1.234
# 0.01562  1.234
# ...
# 64.0     85.2     <- L3 → DRAM transition visible
```

### 3.2 Custom pointer-chase implementation

```c
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define SIZE (256 * 1024 * 1024)  // 256 MB

uintptr_t *buf;

void build_random_chain(size_t n) {
    size_t i;
    for (i = 0; i < n; i++) buf[i] = (uintptr_t)&buf[i];
    // Fisher-Yates shuffle creates random chain
    for (i = n - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        uintptr_t tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
    }
}

uint64_t measure_chase(uintptr_t *start, size_t iterations) {
    struct timespec t0, t1;
    uintptr_t *p = start;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < iterations; i++)
        p = (uintptr_t *)*p;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    // prevent dead-code elimination
    if (p == NULL) abort();
    return (t1.tv_sec - t0.tv_sec) * 1000000000ULL +
           (t1.tv_nsec - t0.tv_nsec);
}
```

### 3.3 Running latency benchmarks correctly

```bash
# Disable frequency scaling
sudo cpupower frequency-set --governor performance

# Pin to specific CPU
taskset -c 0 ./pointer_chase

# Pin to NUMA node 0 memory
numactl --cpunodebind=0 --membind=0 ./pointer_chase

# Compare local vs remote NUMA
numactl --cpunodebind=0 --membind=1 ./pointer_chase
```

---

## 4. Hardware-Software Interaction

The latency curve from a pointer-chase benchmark reveals:
1. flat region at L1 size — hits in L1
2. step up at L1 boundary — transition to L2
3. step up at L2 boundary — transition to L3
4. step up at L3 boundary — transition to DRAM
5. plateau in DRAM region — sustained DRAM latency

Step heights and positions characterize the entire memory hierarchy.

---

## 5. Interview Q and A

Q1: Why does sequential access give misleading latency numbers?  
Hardware prefetchers fetch ahead, hiding actual access latency.

Q2: How does the pointer chain defeat prefetchers?  
Each load's target is data-dependent on the previous load, preventing speculation.

Q3: Why use a random shuffle instead of a sequential chain?  
Sequential chain still has predictable spatial locality; random shuffle eliminates spatial prefetching.

Q4: What does a flat plateau in the DRAM region mean?  
Working set exceeds all caches; every access pays full DRAM latency.

Q5: How would you measure remote NUMA latency?  
Pin CPU to one node, allocate memory on another node, then run the same chase.

Q6: What confounds latency measurement on ARM64 specifically?  
DVFS, big.LITTLE core type differences, and contention from background tasks.

---

## 6. Pitfalls and Gotchas

- Forgetting to disable frequency scaling — latencies vary with current frequency.
- Running on a big.LITTLE little core when expecting big core results.
- Using a small working set that fits in L1 throughout the test.
- Compiler optimizing the loop away — always use the result.

---

## 7. Quick Reference Table

| Working Set | Expected Region | Typical Latency |
|---|---|---|
| 16 KB | L1D | 1-2 ns |
| 256 KB | L2 | 3-5 ns |
| 4 MB | L3 | 10-17 ns |
| 64 MB | DRAM (local) | 67-100 ns |
| 64 MB (remote) | DRAM (remote NUMA) | 133-200 ns |
