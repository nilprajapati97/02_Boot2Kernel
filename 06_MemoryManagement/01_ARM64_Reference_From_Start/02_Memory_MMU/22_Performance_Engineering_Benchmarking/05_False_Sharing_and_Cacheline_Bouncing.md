# False Sharing and Cacheline Bouncing

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

False sharing occurs when threads on different cores modify independent variables that happen to reside in the same cache line.

Effect: cache line bounces between cores' L1 caches due to coherence protocol, causing severe performance degradation even though no logical sharing exists.

---

## 2. ARM64 Hardware Detail

### 2.1 Cache line size on ARM64

Most ARM64 cores use 64-byte cache lines. Some server cores (e.g., Apple, certain Neoverse variants) use 128-byte lines for L2 or beyond.

Detect at runtime:
```bash
cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
```

Use `getconf LEVEL1_DCACHE_LINESIZE` for portable code.

### 2.2 Coherence protocol behavior

ARM uses MESI-class protocols (with MOESI variants per implementation).

False sharing pathology:
1. core A writes variable X (line L) → L is in Modified state on A
2. core B writes variable Y (also in line L) → L invalidated on A
3. core A writes X again → line bounced back from B
4. repeat → cache line ping-pongs continuously

Each bounce costs hundreds of cycles plus interconnect traffic.

---

## 3. Linux Kernel Implementation

### 3.1 Detection with perf

```bash
# Sample on coherence-related events
perf c2c record ./workload
perf c2c report

# Manual: high LL_CACHE_MISS with low LL_CACHE access count
# combined with high cross-core stall is suspicious
```

### 3.2 Kernel idioms to prevent false sharing

```c
// Pad to cache line
struct percpu_data {
    long counter;
    char pad[64 - sizeof(long)];
};

// Or use compiler alignment
struct percpu_data {
    long counter;
} __attribute__((aligned(64)));

// Linux kernel macro
#include <linux/cache.h>
struct foo {
    int hot_field;
} ____cacheline_aligned;

// Per-CPU section split
struct shared {
    spinlock_t lock;
    int counter;
    ____cacheline_aligned int read_mostly_field;
};
```

### 3.3 Detection example with cache miss profiling

```bash
# Build with debug symbols
gcc -g -O2 -pthread workload.c -o workload

# Profile cache contention
perf record -e cache-misses,cache-references --call-graph dwarf -- ./workload
perf report

# Look for symbols with high miss rate per access
```

### 3.4 perf c2c (cache-to-cache) analysis

`perf c2c` is purpose-built for false sharing detection:
- captures HITM (Hit Modified) events
- shows which cache lines bounce between cores
- attributes lines to specific code locations and data structures

---

## 4. Hardware-Software Interaction

The coherence protocol that makes correctness possible also creates the bouncing pathology. Hardware provides correctness; software must structure data to avoid pathological access patterns.

On large ARM64 systems with deep cache hierarchies and complex interconnects, false sharing penalties scale with core count and physical distance between cores.

---

## 5. Interview Q and A

Q1: What is false sharing in one sentence?  
Independent data sharing a cache line causes coherence traffic as if it were truly shared.

Q2: How do you detect false sharing in production code?  
`perf c2c` is the standard tool; also high HITM event rates correlate with false sharing.

Q3: What is the kernel idiom to prevent it?  
`____cacheline_aligned` attribute or explicit padding to cache-line size.

Q4: Why is per-CPU data immune to false sharing?  
Each CPU's instance is allocated separately and aligned to its own cache line.

Q5: Does false sharing affect read-only data?  
No — coherence traffic only triggers on writes. Read-only sharing is fine.

Q6: How does cache line size affect padding strategy?  
Padding must match the largest line size in the coherence domain (often 64B on ARM64, 128B on some).

---

## 6. Pitfalls and Gotchas

- Padding to 64B when target ARM64 platform uses 128B lines.
- Assuming false sharing only matters for hot atomic counters — it affects any concurrent writes.
- Adding padding everywhere without measurement — wastes memory and L1 capacity.
- Ignoring read-mostly fields that occasionally see writes — they can still bounce.

---

## 7. Quick Reference Table

| Symptom | Likely Cause | Fix |
|---|---|---|
| high HITM events | true or false sharing | use `perf c2c` to disambiguate |
| performance drops with thread count | shared cache line contention | pad and align |
| atomic counters slow under load | per-CPU contention | use per-CPU counters with periodic merge |
| read-mostly field slow | accidental colocation with writer | move to its own cache line |
