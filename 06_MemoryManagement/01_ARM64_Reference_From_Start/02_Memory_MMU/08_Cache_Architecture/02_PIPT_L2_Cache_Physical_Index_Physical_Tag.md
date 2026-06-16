# PIPT L2+ Cache: Physical Index Physical Tag

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. PIPT Overview

```
PIPT (Physically Indexed, Physically Tagged):
  Both index (set selection) and tag comparison use PHYSICAL addresses
  
  Lookup procedure:
    1. Virtual address arrives at cache controller
    2. TLB translates VA → PA (must complete before cache lookup)
    3. Use PA bits as cache index to select set
    4. Compare stored tags against PA tag bits
    5. On hit: return data; On miss: fetch from next level
    
  No aliasing possible:
    Physical address is unique per physical location
    Two VAs mapping same PA → same PA → same cache index and tag
    → Same cache line → coherent view automatically
    
  Used for:
    All ARM64 L2 caches (unified, per-core)
    All ARM64 L3/system caches (shared across cores)
    Eliminates aliasing without per-way size constraints
```

---

## 2. PIPT Cache Indexing Math

```
PIPT cache set/tag calculation:

Given PIPT cache parameters:
  S = number of sets (must be power of 2)
  W = number of ways (associativity)
  L = line size in bytes (must be power of 2)
  
  Cache size = S × W × L

Physical address breakdown (64-bit PA on ARM64):
  PA[b-1:0]    = offset within cache line, where b = log2(L)
  PA[b+s-1:b]  = cache set index, where s = log2(S)
  PA[47:b+s]   = tag bits (compared to stored tags)
  
Example: Cortex-A78 L2 (512KB, 8-way, 64B lines):
  L = 64B → b = 6  (PA[5:0] = byte offset)
  S = 512KB / (8 × 64B) = 1024 sets → s = 10  (PA[15:6] = set index)
  Tag = PA[47:16]
  
  Cache lookup:
    1. TLB gives PA
    2. PA[15:6] selects one of 1024 sets
    3. All 8 ways in that set: compare tags against PA[47:16]
    4. Hit if any way matches
    5. Parallel: read all 8 data lines simultaneously, mux based on hit way

Example: Neoverse N2 L2 (1MB per-core, 16-way, 64B lines):
  S = 1MB / (16 × 64B) = 1024 sets
  PA[5:0] = byte offset, PA[15:6] = set index, PA[47:16] = tag
  (same layout, more ways = higher associativity for conflict-miss reduction)
```

---

## 3. L2 Cache: Physical Unification and Coherency

```
L2 as Unification Point:
  PoU (Point of Unification): the level where instruction and data caches
  are unified — data in I-cache and D-cache are coherent with each other.
  
  On ARM64 with Harvard L1 (separate I and D):
    L1 I-cache: caches instructions only
    L1 D-cache: caches data only (also holds code if loaded as data)
    L2: UNIFIED cache (holds both instructions and data)
    
    PoU = L2 for most ARM64 CPUs (CLIDR_EL1.LoUIS=2 → L2 is inner-shareable PoU)
    
  Why this matters:
    JIT compiler: writes code to memory via D-cache (data write)
    CPU executes code via I-cache
    Without PoU synchronization: I-cache sees STALE old instructions!
    
    Fix: DC CVAU (Clean to PoU), IC IVAU (Invalidate I-cache to PoU)
    After JIT writes: DC CVAU → IC IVAU → DSB → ISB
    This ensures L2 (PoU) has the new code → I-cache sees it on next fetch

PoC (Point of Coherency):
  The level where ALL observers (CPU, GPU, DMA devices) are coherent.
  Usually: DRAM or L3 system cache (if the SoC has a coherent interconnect)
  
  CLIDR_EL1.LoC = 3 → L3 is LoC (Level of Coherency)
  
  DMA operations must reach PoC:
    Device reads PA via DMA → must see CPU's latest write (must reach PoC)
    DC CIVAC (Clean+Invalidate to PoC): pushes data from all CPU caches to PoC
```

---

## 4. L2 Cache Performance and Latency

```
L2 cache latency breakdown (Cortex-A78 at 2.8 GHz):
  L2 hit: 10–12 cycles (3.6–4.3 ns)
    - Tag lookup: 3 cycles
    - Data read: 7 cycles
    Total: 10–12 cycles
    
  L1 miss → L2 hit (combined): 12–14 cycles
    - L1 miss detection: 1 cycle
    - L1 → L2 request: 2 cycles
    - L2 tag + data: 10 cycles
    Total: 12–14 cycles

L2 cache conflict misses:
  Even with 8-way associativity, if > 8 cache lines map to the same set,
  one is evicted despite other sets being empty.
  
  Example: Array stride = S bytes, S × 1024 = L2 cache size
    Accessing array[0], array[S/64], array[2S/64] ...
    Each element maps to SAME L2 set (stride = cache size / ways)
    → Only 8 elements can be cached simultaneously (8-way)
    → Elements beyond 8 evict earlier elements = thrashing
    
  Fix: padding, restructuring data, changing access pattern
  Detection: perf stat → high L2_CACHE_REFILL vs L2_CACHE_ACCESS ratio

L2 bandwidth:
  Cortex-A78 L2: 256 bits/cycle bandwidth (32 bytes per cycle)
  At 2.8 GHz: 89.6 GB/s L2 bandwidth
  L1 miss rate × data access rate must stay below this or L2 becomes bottleneck
```

---

## 5. L3 / System Cache (LLC)

```
ARM SoC LLC (Last Level Cache):
  Often called "System Level Cache" or "SLC"
  Shared across all CPUs in the cluster (and sometimes GPU)
  
  Purpose:
    1. Reduce DRAM bandwidth: L3 hit avoids DRAM fetch
    2. CPU-GPU coherency: if GPU accesses CPU memory through LLC
    3. CCI/NoC (Cache Coherent Interconnect) maintains coherency at LLC level
    
  ARM CoreLink / CI-700:
    The LLC is part of the coherent interconnect
    All ARM64 clusters' L2 caches are snooped via the interconnect
    
  LLC characteristics:
    Qualcomm Snapdragon 8 Gen 2: 8MB LLC (System Cache)
    Apple M2: 8MB L2 (unified per cluster) + 24MB L3
    Neoverse V2: 32MB L3 per socket (64 cores)
    
  LLC miss → DRAM:
    LLC miss rate × memory access frequency determines DRAM bandwidth pressure
    Modern SoCs: LPDDR5X bandwidth = 68 GB/s
    If LLC miss rate > 10%: memory bandwidth often becomes the bottleneck

ARM ACE (AXI Coherency Extensions):
  Protocol for coherent interconnect between CPUs and GPU
  L3 serves as "Point of Coherency" for ACE transactions
  DC CIVAC: flushes to LLC (PoC) → visible to DMA and GPU via ACE/ACE-Lite

Cache partitioning (ARM Way Partitioning, SLC features):
  Advanced SoCs partition LLC ways between CPU and GPU:
  CPU: 6 ways, GPU: 2 ways (out of 8 total)
  Prevents GPU workload from thrashing CPU data (and vice versa)
  Linux CDC (Cache Pseudo-Lock) or platform-specific drivers manage this
```

---

## 6. Interview Questions & Answers

**Q1: Why don't we use PIPT for the L1 D-cache if it avoids aliasing?**

PIPT requires the physical address (PA) to compute the cache set index, which means we must **wait for TLB translation to complete** before even starting the cache lookup. TLB lookup takes 1–3 cycles. Using PIPT for L1 D-cache would add these cycles to EVERY memory access, significantly hurting performance in the critical path (L1 cache hit latency would increase from ~4 cycles to ~6–7 cycles).

VIPT allows the cache set lookup to begin **simultaneously** with TLB lookup using virtual address bits (which are known immediately). The physical tag from the TLB is used only for the final tag comparison, which happens AFTER both the TLB and cache set lookup complete. As long as the VIPT doesn't alias (per-way size ≤ page size), this gives identical correctness to PIPT with 1–2 cycle better latency on the critical path.

L2 and higher caches use PIPT because: (a) the extra TLB latency is hidden inside the L1 miss penalty — the CPU is already waiting for L1 miss handling, so 1–2 more cycles for TLB are irrelevant; and (b) L2 is shared or has aliasing risks that PIPT naturally eliminates without any OS constraints.

---

## 7. Quick Reference

| Cache | Indexing | Tagging | Aliasing? | Notes |
|---|---|---|---|---|
| L1 I-cache | Virtual | Physical | Possible | Avoid via OS |
| L1 D-cache | Virtual | Physical | Possible | Avoided by per-way ≤ page size |
| L2 Unified | Physical | Physical | No | PIPT, PoU on ARM64 |
| L3/LLC | Physical | Physical | No | PIPT, PoC on ARM64 |

| L2 Parameter | Cortex-A55 | Cortex-A78 | Neoverse N2 |
|---|---|---|---|
| Size | 128–256 KB | 512KB–2MB | 2MB |
| Ways | 8 | 8–16 | 8 |
| Latency | 8 cycles | 10–12 cycles | 15 cycles |
| Line size | 64B | 64B | 64B |
