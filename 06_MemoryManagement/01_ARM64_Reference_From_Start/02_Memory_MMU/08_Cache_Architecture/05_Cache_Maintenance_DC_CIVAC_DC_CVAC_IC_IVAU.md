# Cache Maintenance Instructions: DC/IC Operations Deep Dive

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Cache Maintenance Instruction Taxonomy

```
ARM64 cache maintenance instructions:

Format: DC/IC <op>, <Xn>    (Data Cache / Instruction Cache)
  where <op> = [scope][type][level]

Scope:
  VA  = Virtual Address (of the specific cache line to operate on)
  SW  = Set/Way (by cache set and way number — internal maintenance)

Type:
  I   = Invalidate (discard dirty data without writing back)
  C   = Clean (write dirty data to next level, keep line valid)
  CI  = Clean + Invalidate (write dirty data AND discard from cache)
  Z   = Zero (fill cache line with zeros — ARM8.8 DC GVA)

Level (Point of Operation):
  PoU = Point of Unification (usually L2, the I+D unification point)
  PoC = Point of Coherency (usually LLC/DRAM, the system-wide coherency point)

Instruction Cache:
  IC IALLU    = I-cache Invalidate ALL to PoU (EL1/EL2 only)
  IC IALLUIS  = I-cache Invalidate ALL to PoU, Inner Shareable (broadcast to all CPUs)
  IC IVAU, Xn = I-cache Invalidate VA to PoU (per-line, for JIT/code patching)

Data Cache:
  DC IVAC, Xn  = Invalidate VA to PoC (discard, DMA from device before CPU read)
  DC CVAC, Xn  = Clean VA to PoC (write back, DMA to device after CPU write)
  DC CIVAC, Xn = Clean+Invalidate VA to PoC (both — bidirectional DMA)
  DC CVAU, Xn  = Clean VA to PoU (code patching — push data to L2 where I-cache sees it)
  DC CISW, ...  = Clean+Invalidate by Set/Way (full cache flush, boot/shutdown only)
  DC ZVA, Xn   = Zero cache line at VA (fast memset, avoids read-modify-write)
```

---

## 2. Point of Unification (PoU) Operations

```
PoU = where Instruction Cache and Data Cache are unified
      (the first cache level that is shared between I and D paths)
      
On most ARM64 CPUs with Harvard L1:
  L1 I-cache: separate instruction cache
  L1 D-cache: separate data cache
  L2: unified (holds both instructions and data)
  → PoU = L2

When PoU operations are needed:
  Any time data written via D-cache needs to be EXECUTED via I-cache:
  1. JIT compilation: write machine code to memory (via D-cache write)
  2. Code patching: kernel modifies a jump instruction
  3. Module loading: ELF text section written to memory
  4. Signal trampoline: kernel writes user return code to stack

JIT flush sequence (mandatory):
  // Step 1: Ensure D-cache has our written code
  for (va = start; va < end; va += dcache_line_size)
      DC CVAU, va        // Clean D-cache line to PoU (push to L2)
  DSB ISH               // Wait for all DC CVAU to complete
  
  // Step 2: Invalidate I-cache so it fetches fresh code from L2
  for (va = start; va < end; va += icache_line_size)
      IC IVAU, va        // Invalidate I-cache line at PoU
  DSB ISH               // Wait for IC IVAU to complete
  ISB                   // Flush instruction pipeline (in-flight instructions)
  
  // Now: executing code from [start, end) uses newly written instructions
  
Linux implementation (arch/arm64/mm/cache.S):
  flush_icache_range(start, end):
    DC CVAU loop + DSB ISH + IC IVAU loop + DSB ISH + ISB
    
  __flush_cache_user_range(start, end):
    Same but handles user-space addresses (with EL0 access checks)

Optimization: CTR_EL0.IDC and DIC bits
  CTR_EL0.IDC = 1: D-cache coherent with I-cache at PoU automatically
    → DC CVAU not needed before IC IVAU (D→PoU coherency is free)
    Linux checks this: if IDC=1, skip DC CVAU in flush_icache_range()
    
  CTR_EL0.DIC = 1: I-cache invalidation for PoU not needed after D-cache clean
    → IC IVAU not needed (hardware maintains I/D coherency at PoU)
    Linux checks this: if DIC=1, skip IC IVAU in flush_icache_range()
    
  Typical ARM64 CPUs (Cortex-A55+): IDC=1, DIC=0
    DC CVAU can be skipped; IC IVAU still needed
```

---

## 3. Point of Coherency (PoC) Operations

```
PoC = the memory level visible to ALL system agents (CPUs, GPU, DMA, etc.)
      Usually: DRAM or system-level cache with coherency for non-CPU devices

DC CVAC, Xn — Clean VA to PoC:
  Writes dirty cache line data to PoC (DRAM or LLC)
  Cache line remains VALID in L1/L2 (just written back, not evicted)
  
  Use case: CPU writes data, DMA engine needs to READ it
    1. CPU writes: D-cache line marked dirty (Modified state)
    2. DMA starts: device reads via DMA → bypasses CPU caches → reads DRAM
    3. Without CVAC: DRAM has stale data! Device reads wrong value.
    4. With CVAC: dirty data forced to PoC → DRAM updated → device reads correct data

DC IVAC, Xn — Invalidate VA to PoC:
  Marks cache line as Invalid (discards, even if dirty — data LOST!)
  Forces next access to fetch from PoC (DRAM)
  
  WARNING: Only use IVAC when you are CERTAIN no dirty data exists in cache,
  OR when you WANT to discard any CPU-written data (replace with device data).
  
  Use case: DMA engine writes to memory, CPU needs to READ fresh device data
    1. Device writes via DMA to DRAM
    2. CPU reads memory: may HIT stale L1/L2 cache copy from before DMA
    3. Without IVAC: CPU reads old cached data, not the device-written data!
    4. With IVAC: cache line invalidated → next CPU read fetches from DRAM = fresh data
  
  Linux: __dma_inv_area() calls DC IVAC per cache line

DC CIVAC, Xn — Clean+Invalidate VA to PoC:
  First writes dirty data to PoC, then marks line as Invalid
  Safe for ALL scenarios (both directions)
  
  Use case: bidirectional DMA, or any DMA where dirty CPU data might exist
  Linux: __dma_flush_area() calls DC CIVAC per cache line
  
  Always use CIVAC when unsure — IVAC on dirty data causes data loss!
  
  Cost comparison:
    DC CIVAC: ~10–30 cycles (write + invalidate per line)
    DC CVAC:  ~5–15 cycles (write only per line)
    DC IVAC:  ~3–8 cycles (invalidate only, no write)
    Best performance: use CVAC or IVAC as appropriate; avoid CIVAC when possible
```

---

## 4. Set/Way Cache Operations (Boot Only)

```
DC CISW, x0 — Clean+Invalidate by Set/Way:
  Operates on a SPECIFIC cache set + way (not by VA)
  Identified by: {level[3:1], way[log2(assoc)-1:0], set[log2(sets)-1:0]}
  
  Register format for DC CISW:
    Bits[3:1]   = Level (0=L1, 1=L2, 2=L3)
    Bits[31:32-A] = Way number (A = log2(associativity))
    Bits[32-A-1:B] = Set number (B = log2(line_size/4))
  
  Example: 4-way L1 D-cache, 64 sets, 64B lines:
    Way[1:0], Set[5:0] → iterate over all 4×64 = 256 combinations
  
  Use: full cache flush at boot, OS shutdown, cache test
  Linux: flush_cache_all() in early boot, before DCE (disable cache+MMU)
  
  NEVER USE in normal runtime:
    Set/way operations are NOT coherent — they only flush the LOCAL core's cache
    On SMP: another core may still have dirty data in its cache
    For SMP: use DC CIVAC (VA-based, coherent) not DC CISW
```

---

## 5. DC ZVA — Zero Cache Line

```
DC ZVA, Xn — Zero (fill with zeros) cache line at VA:
  Sets ALL bytes in the cache line containing VA to zero
  Does NOT perform a memory read first (avoids read-bandwidth for zeroing)
  
  Size: cache line size from DCZID_EL0[3:0] = log2(block_size/4)
    Typical: 4 → block = 4 × 2^4 = 64 bytes (standard cache line)
  
  Advantage vs memset:
    STR instructions: read-modify-write to cache (if line not exclusive: fetches from DRAM)
    DC ZVA: allocates cache line directly as zeros (no fetch from DRAM)
    Performance: DC ZVA reduces both read AND write bandwidth for zeroing
    
  Use in kernel:
    clear_page() uses DC ZVA for fast page zeroing (new page allocation)
    memset optimized library (arm_optimized_routines) uses DC ZVA for large memset(0)
  
  Limitation:
    Only works for zeroing (cannot set arbitrary values)
    DCZID_EL0[4] = DZP (prohibit DC ZVA in EL0): some systems disable this
    Cannot be used on Device or Non-cacheable Normal memory
    Line must be writeable (AP must permit write)
```

---

## 6. Linux Cache Maintenance Functions

```
arch/arm64/mm/cache.S — key functions:

flush_cache_all():
  DC CISW for all sets/ways of L1 D-cache
  Used: arch_kexec_protect_crashkres(), kexec, early boot
  
flush_icache_range(start, end):
  DC CVAU [start, end) + DSB ISH
  IC IVAU [start, end) + DSB ISH + ISB
  Used: module loading, ftrace patching, alternatives patching

__flush_dcache_area(kaddr, size):
  DC CIVAC for [kaddr, kaddr+size)
  Used: DMA operations, sync between cores

__inval_dcache_area(kaddr, size):
  DC IVAC for [kaddr, kaddr+size)
  Used: DMA from device to memory (invalidate before CPU read)

__clean_dcache_area_poc(kaddr, size):
  DC CVAC for [kaddr, kaddr+size) + DSB ISH
  Used: Flush to PoC before DMA reads

__clean_dcache_area_pop(kaddr, size):
  DC CVAP for [kaddr, kaddr+size) + DSB ISH
  (CVAP = Clean to PoP: Point of Persistence — NVDIMM/persistent memory)

cache_line_size() macro:
  Returns CTR_EL0.DminLine shifted to bytes
  Used in loop increment: va += cache_line_size()
```

---

## 7. Interview Questions & Answers

**Q1: When should you use DC IVAC vs DC CIVAC for DMA operations?**

Use `DC IVAC` (Invalidate only) when: the DMA direction is `DMA_FROM_DEVICE` — the device is WRITING to memory, and you need the CPU to see fresh device data. Since the device will overwrite the memory, any existing dirty CPU data in the cache would be discarded correctly — the device's write makes the CPU's cached data irrelevant. However, `DC IVAC` on a dirty line **discards the dirty data without writing it back** — so if the CPU had written new data to this region since the last writeback, that data is LOST. This is why `DC IVAC` must only be used when you're certain the CPU hasn't dirtied the region, OR you explicitly don't care about any prior CPU writes.

Use `DC CIVAC` (Clean+Invalidate) for `DMA_BIDIRECTIONAL` — when the device may read AND write. Clean ensures any dirty CPU data reaches DRAM (device sees it), then Invalidate ensures the CPU will fetch fresh device data after the DMA. It's safer but more expensive. For `DMA_TO_DEVICE`, use `DC CVAC` (Clean only) — the device reads from DRAM, so dirty CPU data must reach DRAM, but the cache line can remain valid in the CPU cache (the device won't modify the data).

---

## 8. Quick Reference

| Instruction | Operation | When to Use |
|---|---|---|
| `DC CVAC, Xn` | Clean to PoC | DMA_TO_DEVICE: CPU writes, device reads |
| `DC IVAC, Xn` | Invalidate at PoC | DMA_FROM_DEVICE: device writes, CPU reads |
| `DC CIVAC, Xn` | Clean+Inv at PoC | DMA_BIDIRECTIONAL: both directions |
| `DC CVAU, Xn` | Clean to PoU | JIT/code patch: D-cache → I-cache |
| `IC IVAU, Xn` | Invalidate I-cache at PoU | JIT/code patch: after DC CVAU |
| `IC IALLUIS` | Inv all I-cache IS | Full I-cache flush across all CPUs |
| `DC ZVA, Xn` | Zero cache line | Fast page zeroing (clear_page) |
| `DC CISW, x0` | Clean+Inv by set/way | Boot-time full cache flush ONLY |

| CTR_EL0 bit | Meaning | Linux optimization |
|---|---|---|
| IDC [28] | D-cache coherent with I-cache at PoU | Skip DC CVAU in flush_icache_range |
| DIC [29] | I-cache inval not needed at PoU | Skip IC IVAU in flush_icache_range |
