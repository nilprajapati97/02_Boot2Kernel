# Memory Bandwidth Benchmarking — STREAM and Beyond

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Memory bandwidth benchmarking measures the rate at which a system can move data between CPU and main memory.

STREAM is the de facto standard, but modern ARM64 systems require careful methodology to expose true sustained bandwidth across cores, NUMA nodes, and access patterns.

---

## 2. ARM64 Hardware Detail

### 2.1 Bandwidth-relevant architecture

- DRAM channels per socket (varies by SoC)
- coherent interconnect bandwidth (CMN topology)
- per-core memory request queue depth
- prefetcher behavior under sequential vs strided access
- LLC eviction policy effects on streaming workloads

### 2.2 Theoretical vs achievable bandwidth

Theoretical: channel count × data rate × bus width  
Achievable: typically 60–85% of theoretical depending on:
- access pattern
- thread count
- write-allocate behavior
- coherence protocol overhead

---

## 3. Linux Kernel Implementation

### 3.1 STREAM benchmark structure

Four kernels:
- COPY: `a[i] = b[i]` — 2 streams (1 read, 1 write)
- SCALE: `a[i] = q * b[i]` — 2 streams + compute
- ADD: `a[i] = b[i] + c[i]` — 3 streams (2 reads, 1 write)
- TRIAD: `a[i] = b[i] + q * c[i]` — 3 streams + compute

### 3.2 Running STREAM correctly on ARM64

```bash
# Build with appropriate flags
gcc -O3 -fopenmp -mcpu=native -DSTREAM_ARRAY_SIZE=100000000 \
    -DNTIMES=20 stream.c -o stream

# Pin to NUMA node and run
numactl --cpunodebind=0 --membind=0 ./stream

# Multi-node bandwidth
numactl --interleave=all ./stream

# Vary thread count to find scaling cliff
for t in 1 2 4 8 16 32 64; do
  OMP_NUM_THREADS=$t numactl --cpunodebind=0 --membind=0 ./stream
done
```

### 3.3 Common methodology errors

- array too small (fits in cache) — measures cache bandwidth, not memory
- single-threaded only — misses peak achievable
- frequency scaling enabled — variable results
- using `malloc` without first-touch — pages allocated on wrong NUMA node

### 3.4 Beyond STREAM: more realistic benchmarks

- **lmbench bw_mem** — varied transfer sizes
- **mlc (Intel Memory Latency Checker)** — ARM equivalents from vendors
- **bandwidth** (Zack Smith's tool) — wider access pattern coverage
- **GUPS (Giga-Updates Per Second)** — random access bandwidth (HPC standard)

---

## 4. Hardware-Software Interaction

STREAM exposes the combination of:
- DRAM controller efficiency
- coherent fabric throughput
- cache line allocation policy
- prefetcher contribution

A 2x discrepancy between two ARM64 SoCs at the same DRAM speed usually points to interconnect or controller differences, not raw memory speed.

---

## 5. Interview Q and A

Q1: Why is TRIAD the most-quoted STREAM result?  
It exercises 3 streams with compute, representing realistic compute-memory balance.

Q2: What does "achievable" bandwidth depend on most?  
Thread count, NUMA placement, and read/write ratio of the access pattern.

Q3: Why interleave memory across nodes?  
To exercise full system bandwidth rather than single-node ceiling.

Q4: What is first-touch policy and why does it matter?  
Pages are allocated on the NUMA node of the first writer; without proper initialization, all data ends up on one node.

Q5: When is STREAM misleading?  
For workloads with random access or small footprint — measure those separately.

Q6: What is GUPS measuring that STREAM is not?  
Random access bandwidth, which exposes latency-hiding effectiveness and prefetcher limits.

---

## 6. Pitfalls and Gotchas

- Reporting STREAM TRIAD without specifying thread count, array size, and NUMA setup.
- Building without `-O3` and CPU-specific flags — leaves bandwidth on the table.
- Comparing results across kernels without identical methodology.
- Using `malloc` arrays without first-touch initialization on the target NUMA node.

---

## 7. Quick Reference Table

| Kernel | Streams | What It Measures |
|---|---|---|
| COPY | 2 (1R, 1W) | basic read-write bandwidth |
| SCALE | 2 + compute | bandwidth with multiply |
| ADD | 3 (2R, 1W) | multi-stream read bandwidth |
| TRIAD | 3 + compute | realistic combined load (most quoted) |
