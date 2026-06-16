# ARM64 Cache Architecture: L1/L2/L3 Hierarchy

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Cache hierarchy exists because DRAM latency (40–100 ns) is 100–300× the CPU cycle time
(0.3–1 ns at 1–3 GHz). Without caches, every memory access would stall the CPU for
100–300 cycles, reducing a 3 GHz CPU to effectively 10–30 MHz throughput.

Cache hierarchy trades cost/capacity for latency:
  L1 cache: small (32–64 KB), very fast (4 cycles), per-core
  L2 cache: medium (256KB–4MB), moderate (12–20 cycles), per-core or shared
  L3 cache: large (4–64 MB), slower (30–50 cycles), shared across cores
  DRAM: unbounded, slow (100–300 cycles)
  
Cache effectiveness:
  Principle of temporal locality: recently accessed data likely accessed again soon
  Principle of spatial locality: data near recently accessed data likely accessed soon
  Both principles: caches exploit these → high hit rates (>99% for typical workloads)
```

---

## 2. ARM64 Cache Architecture: Hardware Detail

```
ARM64 Cache Identification System Register: CTR_EL0
  Bits[3:0]  = IminLine: L1 I-cache line size = 4 × 2^IminLine bytes
  Bits[19:16] = DminLine: L1 D-cache line size = 4 × 2^DminLine bytes
  Bit[28]   = IDC: I-cache invalidation to PoU not required for data to code coherency
  Bit[29]   = DIC: Data cache invalidation not required for code execution at PoU
  Bits[27:24] = CWG: Cache Writeback Granule = 4 × 2^CWG bytes
  
  Read cache line size in Linux:
    dcache_line_size = 1 << CTR_EL0.DminLine+2   (typically 6 → 64 bytes)
    icache_line_size = 1 << CTR_EL0.IminLine+2   (typically 6 → 64 bytes)

CCSIDR_EL1 (Cache Size ID Register):
  Access procedure:
    1. Write CSSELR_EL1: select which cache level and type to query
       CSSELR_EL1.Level[3:1] = 0 (L1), 1 (L2), 2 (L3)
       CSSELR_EL1.InD[0] = 0 (data/unified), 1 (instruction)
    2. ISB
    3. Read CCSIDR_EL1
    
  CCSIDR_EL1 fields:
    Bits[2:0] = LineSize: line size = 2^(LineSize+4) bytes (0→16B, 2→64B, 3→128B)
    Bits[12:3] = Associativity: ways-1 (so 3=4-way, 7=8-way, 15=16-way)
    Bits[27:13] = NumSets: sets-1
    
  Cache size = (NumSets+1) × (Associativity+1) × 2^(LineSize+4)
  
  Example Cortex-A78:
    L1 D-cache: 64 sets, 4-way, 64B lines → 64 × 4 × 64 = 16 KB
    L1 I-cache: 64 sets, 4-way, 64B lines → 16 KB
    L2 cache: 2048 sets, 8-way, 64B lines → 1 MB (unified)
    L3 cache: system cache, typically 8–16 MB (SoC-specific)

CLIDR_EL1 (Cache Level ID Register):
  3 bits per cache level describing: no cache, instruction, data, separate I+D, unified
  Bits[2:0] = Level1 type: 0=none, 1=I-only, 2=D-only, 3=separate I+D, 4=unified
  Bits[5:3] = Level2 type
  ...
  Bits[26:24] = LoUIS: Level of Unification Inner Shareable
  Bits[29:27] = LoC: Level of Coherency
  Bits[32:30] = LoUU: Level of Unification Uniprocessor
```

---

## 3. Cache Inclusivity and Exclusivity

```
Inclusive cache (e.g., Cortex-A55, A72):
  L2 CONTAINS copies of all L1 data
  L1 hit → L2 also has a copy (but L1 responds faster)
  L1 eviction: L2 still has the data
  L3 CONTAINS copies of all L2 data
  
  Advantage: simple coherency — checking L3 is sufficient to find any data
  Disadvantage: wastes capacity (each cache block appears at multiple levels)
  
  Example: 
    L1: 32KB; L2: 256KB → L2 effectively only adds 224KB of NEW data
    (32KB is "wasted" replicating L1 content)

Exclusive cache (e.g., AMD L1/L2):
  L1 and L2 are EXCLUSIVE: same cache line cannot be in both
  L1 eviction → line goes to L2 (not discarded)
  L2 eviction → line goes to L3 or DRAM
  
  Advantage: total effective capacity = L1 + L2 (no waste)
  Disadvantage: complex coherency logic

Non-inclusive / NINE (Non-Inclusive Non-Exclusive):
  L2 may or may not have copies of L1 data
  Most modern ARM64 big-LITTLE: L2 is NINE (can have or not have L1 copies)
  
  Cortex-A78 example:
    L1 (32KB, per-core, VIPT): non-inclusive with L2
    L2 (512KB–2MB, per-core, PIPT): non-inclusive with L3
    L3 (System Cache, 4–16MB, shared): non-inclusive / victim cache

ARM SoC (Qualcomm Snapdragon 8 Gen 3 / Oryon):
  Prime core: 8MB L2 (unified!) per-core
  Performance cores: 2MB L2 each
  Efficiency cores: 512KB L2 each
  Shared L3: 12MB system level cache
  These are non-inclusive: each level holds unique data
```

---

## 4. VIPT L1 D-cache and Aliasing

```
VIPT (Virtually Indexed, Physically Tagged):
  Index: virtual address bits used to select cache set (for speed — no TLB needed to index)
  Tag: physical address stored in cache to verify correctness
  
  Why VIPT is used for L1 D-cache:
    Speed: can look up cache set using VA bits SIMULTANEOUSLY with TLB lookup
    (VA bits and PA bits needed simultaneously: VA bits for index, PA for tag comparison)
    This gives 1 cycle faster lookup vs PIPT (Physical Index Physical Tag)
    
  VIPT cache aliasing problem:
    Two different VAs (VA1, VA2) → same PA
    (Shared memory, DMA mappings, KPTI dual-mapping of trampoline)
    
    If VA1 and VA2 use DIFFERENT cache sets (different index bits):
      Data at this PA can appear in TWO different cache sets simultaneously!
      One VA sees cached value A; other VA sees stale value B
      → Cache aliasing bug: inconsistent views of same physical memory
      
  ARM64 fix for VIPT aliasing:
    Cache way size ≤ page size (4KB) when using 12-bit page offset
    
    For 4KB pages: page offset bits[11:0] are the same in VA and PA
    (PA = PTRANSBASE + VA[11:0], so VA[11:0] == PA[11:0])
    
    VIPT index bits (VA bits used to select set) must be within [11:0]:
      If set index uses only bits[11:0] → VA[11:0] == PA[11:0] → no aliasing possible!
    
    Formula: ways × sets × line_size ≤ page_size
      4-way, 64 sets, 64B lines: 4 × 64 × 64 = 16 KB ≤ 4KB? NO!
      
      Wait: only SETS × LINE_SIZE must ≤ PAGE_SIZE (index calculation):
        Sets × LineSize = 64 × 64 = 4096 = 4KB ≤ 4KB ✓
        Sets × LineSize = index range = 4KB = fits in page offset bits[11:0]
        → NO aliasing for Cortex-A78 L1 D-cache 16KB, 4-way!
        
    This is why L1 D-cache size is always ≤ ways × page_size on ARM64:
      16KB 4-way → 16KB/4 = 4KB per way ≤ 4KB page ✓
      32KB 2-way → 32KB/2 = 16KB per way > 4KB page → ALIASING POSSIBLE
      32KB 4-way → 32KB/4 = 8KB per way > 4KB page → ALIASING POSSIBLE
      
    Older ARM cores (Cortex-A9, A15 with 32KB 4-way VIPT L1):
      Per-way size = 8KB > 4KB → aliasing possible
      Fix: page coloring (ensure aliases use same cache sets) → complex OS support
      Modern ARM64 avoids this by using ≤ 4KB per way for L1 D-cache
```

---

## 5. Linux Cache Operations

```
Linux cache maintenance functions (arch/arm64/include/asm/cacheflush.h):

  flush_cache_all():
    DC CISW: clean+invalidate by set/way for all levels (slow, avoid in production)
    Used only during boot, early setup
    
  flush_icache_range(start, end):
    DC CVAU: clean data cache to PoU for range [start, end)
    IC IVAU: invalidate instruction cache to PoU for range [start, end)
    DSB ISH, ISB
    
  __dma_flush_area(start, size):
    DC CIVAC: clean+invalidate for DMA (to PoC)
    Used before DMA_FROM_DEVICE: ensure device sees latest data
    
  __flush_dcache_area(kaddr, size):
    DC CIVAC: clean+invalidate to PoC
    Used for general purpose cache flush
    
  invalidate_icache_range(start, end):
    IC IVAU: invalidate instruction cache
    DSB ISH, ISB
    Used when: code patching, JIT compilation

Cache line size access in kernel:
  cache_line_size() → returns dcache_line_size (from CTR_EL0)
  Typically 64 bytes on ARM64
  Used for: DMA alignment, false sharing avoidance, prefetch distance

False sharing avoidance:
  __cacheline_aligned: align structure to cache line boundary
  __cacheline_aligned_in_smp: align only for SMP (no-op on UP)
  DEFINE_PER_CPU: per-CPU variables avoid false sharing between CPUs
```

---

## 6. Interview Questions & Answers

**Q1: Why does ARM64 use VIPT for L1 D-cache instead of PIPT? What constraint does this impose?**

ARM64 uses VIPT (Virtually Indexed, Physically Tagged) for L1 D-cache primarily for **speed**. A cache lookup requires both an index (to select the cache set) and a tag (to verify the stored line matches the requested address). With PIPT, you must wait for the TLB translation to complete before you can compute the cache index — serializing the two lookups. With VIPT, you use **virtual address bits as the index** and start the cache set lookup immediately in parallel with the TLB, using the physical tag from the TLB result for the final match. This saves 1–2 cycles per access in the critical path.

The constraint: VIPT causes **cache aliasing** if two different virtual addresses map to the same physical address but compute different cache set indices. To prevent this, ARM64 ensures the per-way cache size ≤ page size (4KB). Since `per-way size = total_cache_size / associativity`, a 32KB 4-way L1 D-cache has 8KB per way — which would cause aliasing. ARM64 processors stay at ≤ 4KB per way (e.g., 16KB 4-way = 4KB per way). This means the VIPT index only uses bits[11:0], which are identical in VA and PA (they're the page offset), so aliasing is impossible.

---

## 7. Quick Reference

| Cache Level | Typical Size | Associativity | Latency | Scope |
|---|---|---|---|---|
| L1 I-cache | 32–64 KB | 4–8 way | 2–4 cycles | Per-core |
| L1 D-cache | 16–64 KB | 4–8 way | 4–6 cycles | Per-core |
| L2 Unified | 256KB–8MB | 8–16 way | 8–20 cycles | Per-core |
| L3/LLC | 4–64 MB | 16–32 way | 30–50 cycles | Shared |
| DRAM | Unlimited | N/A | 100–300 cycles | System |

| Cache Type | Index | Tag | Aliasing Risk |
|---|---|---|---|
| VIPT | Virtual | Physical | Yes (if per-way > page size) |
| PIPT | Physical | Physical | No |
| VIVT | Virtual | Virtual | Yes (always) |

| ARM64 Register | Purpose |
|---|---|
| CTR_EL0 | Cache line sizes |
| CLIDR_EL1 | Cache level types + LoC/LoUIS |
| CSSELR_EL1 | Select cache level for CCSIDR query |
| CCSIDR_EL1 | Cache size parameters |
