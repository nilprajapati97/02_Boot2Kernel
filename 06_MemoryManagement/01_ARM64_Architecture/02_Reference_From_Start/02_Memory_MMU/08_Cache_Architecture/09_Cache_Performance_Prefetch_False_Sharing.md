# Cache Performance: Prefetching, False Sharing, Cache Coloring

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Hardware Prefetching on ARM64

```
Hardware prefetcher: automatically fetches cache lines BEFORE the CPU requests them
  Detects: sequential access patterns, stride patterns
  Predicts: next likely access → issues load to L2/L3 before CPU stalls on miss

ARM64 hardware prefetcher types:
  Stream prefetcher:
    Detects: consecutive cache line accesses (array traversal)
    Action: prefetch next N lines ahead of current access
    Trigger: 2–4 consecutive misses in same direction
    Distance: typically 4–16 lines ahead (256B–1KB lookahead)
    
  Stride prefetcher:
    Detects: constant-stride access (struct-of-arrays, matrix rows)
    Action: prefetch at intervals matching detected stride
    Examples: walking linked list nodes (stride = node_size), matrix rows
    
  Context prefetcher (Cortex-X2/Neoverse):
    More sophisticated: tracks access history per PC address
    Can handle irregular patterns that simpler prefetchers miss

Hardware prefetcher effectiveness:
  Sequential array access: ~100% coverage (stream prefetcher handles perfectly)
  Random access (hash tables, trees): ~0% coverage (unpredictable)
  Stride access: 70–90% coverage (if stride is regular)
  
  Impact:
    Perfect prefetch: no L3 miss latency on critical path → hides DRAM latency
    Imperfect: L3/DRAM miss appears as CPU stall → latency visible in perf data

HWPF control registers (Cortex-A78):
  CPUACTLR_EL1: CPU Activity Control Register (implementation-defined)
  Bit[0]: disable L1 stream prefetcher
  Bit[1]: disable L2 stream prefetcher
  Not accessible from EL0 (kernel/EL1 only); firmware may configure
  
  Linux: typically does not disable prefetchers (defaults are good)
  Exception: NUMA-distance benchmarks may disable prefetch to measure true latency
```

---

## 2. Software Prefetch Instructions

```
ARM64 software prefetch: PRFM (PReFeM Memory)

Syntax: PRFM <type>, <address>

Type = <policy><target><action>
  Policy:  PLD (PrefetchLoad), PST (PrefetchStore)
  Target:  L1 (level 1), L2 (level 2), L3 (level 3)
  Action:  KEEP (retain in cache), STRM (streaming, one-time use)

Common prefetch hints:
  PRFM PLDL1KEEP, [x0]    → prefetch for load into L1, keep
  PRFM PLDL2KEEP, [x0]    → prefetch for load into L2
  PRFM PLDL3KEEP, [x0]    → prefetch for load into L3
  PRFM PLDL1STRM, [x0]    → prefetch, streaming (discard after use)
  PRFM PSTL1KEEP, [x0]    → prefetch for store into L1

Prefetch distance calculation:
  Distance = latency × throughput
  
  For L2 miss (fills from L3, ~30 cycles latency) at 2 memory ops/cycle:
    Distance = 30 cycles / 0.5 cycles/line = 60 cache lines = 3840 bytes
    Prefetch 60 lines ahead of current position
    
  For DRAM miss (~200 cycles) at 1 memory op/cycle:
    Distance = 200 lines = 12800 bytes ≈ 12KB ahead
    
Linux usage examples:
  arch/arm64/lib/memcpy.S: PRFM for prefetch in copy loop
  net/core/skbuff.c: prefetch(skb->data) for network receive path
  kernel/sched/fair.c: prefetch next runqueue entry
  
  prefetch(x) macro → expands to: __builtin_prefetch(x, 0, 3) → PRFM PLDL1KEEP
  prefetchw(x) → write prefetch: __builtin_prefetch(x, 1, 3) → PRFM PSTL1KEEP

When software prefetch helps:
  Loop body where next iteration's data is known:
    for (i=0; i<N; i++) {
        prefetch(&array[i + DIST]);   // prefetch ahead
        process(array[i]);             // process current
    }
  
  Where NOT to use prefetch:
    Tight hot loops (CPU already has prefetcher; redundant)
    Unpredictable access patterns (prefetch address unknown at time of issue)
    Very small arrays (fit in cache; no benefit)
```

---

## 3. False Sharing

```
False sharing: two CPU cores access DIFFERENT variables that happen to
reside in the SAME cache line (64 bytes on ARM64).

Result: every write by one core → invalidates the OTHER core's cache line
  Even though the variables are logically independent!
  
Example: global counters struct
  struct stats {
      long cpu0_count;   // offset 0–7
      long cpu1_count;   // offset 8–15
  };
  
  CPU0 increments cpu0_count: modifies cache line → sends Invalidate to CPU1
  CPU1 increments cpu1_count: cache miss! Must re-fetch from CPU0's L1/L2
  CPU0 increments again: ANOTHER miss (CPU1 just invalidated CPU0's line)
  
  This ping-pong continues: each increment by either core requires fetching
  the cache line from the OTHER core's cache.
  Performance: effectively serialized despite being "independent" operations
  
  Measured overhead: up to 100× slower than false-sharing-free code on SMP
  (per-increment cost: 150+ cycles for cache coherency vs 1 cycle with local cache)

Detection:
  perf c2c record -a -- sleep 30   # record cache-to-cache sharing
  perf c2c report
  Output: shows hot cache lines with high cross-CPU sharing rates
  
  Symptom: high REMOTE_HIT count in perf c2c (other CPU's cache hit)
  
  Alternative: Valgrind --tool=dhat (heap layout analysis) for structure layout

Fix 1: per-CPU variables (Linux approach)
  DEFINE_PER_CPU(long, my_counter):
    Each CPU has its own cache-line-aligned copy
    No cross-CPU cache invalidation
    Aggregate: for_each_cpu: total += per_cpu(my_counter, cpu)
  
Fix 2: cache-line padding
  struct stats {
      long cpu0_count;
      long __pad0[7];  // pad to 64 bytes
      long cpu1_count;
      long __pad1[7];  // pad to 64 bytes
  };
  OR use __cacheline_aligned:
  struct stats {
      long cpu0_count __cacheline_aligned;
      long cpu1_count __cacheline_aligned;
  };
  
Fix 3: restructure to avoid sharing
  Instead of shared struct with per-CPU fields:
    Use per_cpu_ptr or percpu_counter (Linux's atomic per-CPU counters)

Linux kernel uses __cacheline_aligned extensively:
  struct task_struct: hot fields cacheline-aligned
  struct rq (run queue): per-CPU, naturally isolated
  atomic_t spinlock: padded to avoid false sharing with adjacent data
```

---

## 4. Cache Prefetch and Alignment Best Practices

```
Structure layout for cache efficiency:

Hot-cold field splitting:
  struct connection {
      /* HOT: accessed every packet */
      uint32_t state;
      uint32_t bytes_sent;
      uint64_t last_active;
      
      char __pad[48];  // fill to 64B cacheline boundary
      
      /* COLD: accessed only on connection setup/teardown */
      char remote_addr[46];
      uint16_t port;
      ...
  };
  
  Hot fields in first cache line: CPU loads one cache line, gets all hot data
  Cold fields in later cache lines: not loaded unless needed

Array-of-Structures vs Structure-of-Arrays:
  AoS (bad for SIMD/prefetch):
    struct Particle { float x, y, z, mass; };
    Particle particles[N];  // x,y,z,mass interleaved
    for(i) { particles[i].x += ...; }  // touches 16B per particle
    
  SoA (better for vectorization + prefetch):
    float x[N], y[N], z[N], mass[N];  // separate arrays
    for(i) { x[i] += ...; }  // sequential 4B per iteration
    Prefetcher: stream pattern → high hit rate
    SIMD: load 4 floats (128-bit NEON): all from x[] = efficient
    
  ARM64 NEON: 128-bit vectors → process 4 floats per instruction
  SoA enables optimal NEON vectorization with excellent prefetch coverage

Cache line size awareness:
  malloc/kmalloc: may allocate objects smaller than cache line (16B, 32B)
  Two objects in same cache line: writing one invalidates the line for other
  For high-concurrency shared objects: ensure each has own cache line
  
  SLAB/SLUB allocator: aligns kmem_cache objects by default for common sizes
  Use SLAB_HWCACHE_ALIGN flag: kmem_cache_create("name", size, 0, SLAB_HWCACHE_ALIGN, NULL)
  → Aligns objects to cache line boundary → prevents false sharing in slab
```

---

## 5. Interview Questions & Answers

**Q1: You have a hot shared data structure accessed by 16 CPU cores. Performance drops 10× compared to single-core. How would you diagnose and fix this?**

**Diagnosis**: Run `perf c2c record -a -- ./workload && perf c2c report`. Look for cache lines with high `HITM` (Hit In Modified state by another CPU) counts. High HITM means cores are constantly fetching cache lines that another core just modified — this is false sharing. Also check `perf stat -e cache-misses,cache-references,LLC-load-misses` to see if L3 miss rate is abnormally high under SMP load.

**Root cause analysis**: Examine the hot data structure. With 16 cores, if any two cores modify fields that share a 64-byte cache line, every write causes MESI invalidations across all 16 cores. The cost per write jumps from ~1 cycle (local hit) to ~50–150 cycles (coherency round-trip).

**Fixes**:
1. **Per-CPU partitioning**: if each core has a "slice" of work, use separate data per core. Use `DEFINE_PER_CPU` or partition arrays as `array[core][...]`.
2. **Cache-line padding**: add `__cacheline_aligned` to separate frequently-mutated fields onto their own cache lines.
3. **Read-mostly replication**: for read-heavy data, replicate it per-CPU. Use RCU (Read-Copy-Update) for update semantics.
4. **Aggregate approach**: instead of a shared counter, use per-CPU counters and aggregate only when needed (like Linux's `percpu_counter`).

---

## 6. Quick Reference

| Prefetch Hint | ARM64 Instruction | Use Case |
|---|---|---|
| Load into L1 | `PRFM PLDL1KEEP` | Hot data needed immediately |
| Load into L2 | `PRFM PLDL2KEEP` | Data needed in 10–30 cycles |
| Streaming load | `PRFM PLDL1STRM` | One-time sequential scan |
| Write into L1 | `PRFM PSTL1KEEP` | About to write (avoid RFO) |

| False Sharing Fix | Best For |
|---|---|
| `DEFINE_PER_CPU` | Counters, per-CPU state |
| `__cacheline_aligned` | Shared structs, spinlocks |
| `SLAB_HWCACHE_ALIGN` | High-frequency kernel objects |
| SoA data layout | Vectorized hot loops |

| Access Pattern | Prefetch Coverage | Recommendation |
|---|---|---|
| Sequential (arrays) | ~100% HW prefetch | Let HW handle; add PRFM only if not keeping up |
| Stride (matrices) | 70–90% HW | Add SW stride prefetch if profiler shows misses |
| Random (trees, hash) | ~0% | Restructure data layout or use prefetch with known depth |
