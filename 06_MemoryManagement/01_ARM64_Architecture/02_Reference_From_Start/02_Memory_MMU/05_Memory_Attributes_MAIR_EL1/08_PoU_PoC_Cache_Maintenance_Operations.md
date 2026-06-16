# Cache Point of Unification (PoU) and Point of Coherency (PoC)

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Overview: Points of Reference

ARM64 defines specific "points" in the memory system that represent where coherency/unification must be observed. These determine which cache maintenance operations are needed and at what granularity.

```
ARM64 Memory System Points:

PoU (Point of Unification):
  The point where the instruction cache, data cache, and page table walks
  for a given PE (Processing Element) must be coherent with each other.
  
  Typically: L2 cache (where I-cache and D-cache join)
  OR: if L1 I-cache and D-cache are both virtually-indexed, PoU may be L1
  
  PoU is PER-PE (per core): each core has its own PoU.
  Coherency at PoU means: one CPU's view of instruction memory is consistent
  with its data memory view.

PoC (Point of Coherency):
  The point where all observers (all CPUs, DMA engines, all cache levels)
  share a single coherent view of memory.
  
  Typically: Last Level Cache (LLC/L3) or DRAM itself.
  When using a coherent interconnect: the interconnect can be the PoC.
  
  PoC is SYSTEM-WIDE: all CPUs and DMA engines see the same value.
  Coherency at PoC means: all system agents read the same data.
```

---

## 2. Why Two Separate Points?

```
ARM64 CPUs typically have a HARVARD ARCHITECTURE for L1:
  Separate L1 Instruction cache (I-cache) and L1 Data cache (D-cache)
  The I-cache and D-cache are independent — NOT automatically coherent!

Problem scenario:
  CPU writes new code (instruction bytes) to DRAM via D-cache.
  D-cache is dirty with new instruction bytes.
  I-cache may still hold old instruction bytes for the same VA.
  
  CPU jumps to the new code address:
    I-cache lookup → OLD bytes still cached → CPU executes OLD code
    (The new instructions are in D-cache, invisible to I-cache!)
    
  This is the "self-modifying code" / "JIT cache flush" problem.

PoU addresses this:
  "Flush D-cache to PoU" for address range → ensures D-cache writes
  propagate to L2 (or wherever D-cache and I-cache share)
  "Invalidate I-cache to PoU" → clear I-cache so next fetch loads
  fresh instructions from L2/L3
  
  After these two operations: I-cache and D-cache agree on instruction bytes.

PoC addresses full system coherency:
  "Flush D-cache to PoC" → data goes all the way to DRAM or LLC
  Needed for: DMA engines that bypass all caches, SMMU, other CPUs
  with separate cache topologies.
```

---

## 3. ARM64 Cache Maintenance Instructions

```
DC (Data Cache) maintenance instructions:
  DC CIVAC, Xt — Clean and Invalidate to PoC by VA to Coherency
    Clean: write dirty line to DRAM (PoC)
    Invalidate: mark line invalid in cache
    Combines both clean and invalidate in one operation
    Use: before DMA engine reads (ensures device sees CPU's writes)

  DC CVAC, Xt  — Clean to PoC by VA to Coherency
    Clean dirty line to DRAM (PoC), keep in cache (still readable)
    Use: when CPU needs to keep data in cache but DMA must see it

  DC CVAU, Xt  — Clean to PoU by VA to Unification
    Clean dirty line to PoU (typically L2)
    Use: for JIT/self-modifying code: flush D-cache to L2 before I-cache sync

  DC IVAC, Xt  — Invalidate to PoC by VA to Coherency
    Invalidate cache line (marks it invalid, does NOT write dirty data)
    DANGEROUS: if line is dirty, dirty data is DISCARDED (data loss!)
    Use ONLY when you know the line is clean, OR after DMA has written DRAM
    Use: after DMA engine writes to DRAM (invalidate CPU cache to force fresh read)

  DC ISW, Xr   — Invalidate by Set/Way (deprecated for software use)
  DC CSW, Xr   — Clean by Set/Way (deprecated)
  DC CISW, Xr  — Clean+Invalidate by Set/Way (deprecated)

  DC ZVA, Xt   — Zero by VA (zero cache line without reading from memory)
                 Useful for zero-init: no RFO needed

IC (Instruction Cache) maintenance instructions:
  IC IALLU    — Invalidate ALL I-cache to PoU (no address, invalidates all)
               Use after patching kernel code, module loading
               
  IC IALLUIS  — Invalidate all I-cache to PoU Inner Shareable
               Broadcasts to all CPUs in ISH domain
               
  IC IVAU, Xt — Invalidate I-cache by VA to PoU
               Precise: invalidate specific address range
               Use: for JIT, signal handlers, dynamic code
```

---

## 4. Flush Sequence for JIT Compilation

```
A JIT compiler generates new machine code in a writable buffer,
then executes it. The mandatory flush sequence:

1. CPU writes machine code bytes to buffer (via D-cache, WB policy):
   memcpy(jit_code_buf, generated_bytes, code_size);

2. Clean D-cache to PoU (propagate writes to L2 where I-cache can see):
   for (addr = start; addr < end; addr += CACHE_LINE_SIZE)
       asm("dc cvau, %0" :: "r"(addr) : "memory");
   asm("dsb ish" ::: "memory");   // Ensure all clean ops complete

3. Invalidate I-cache at PoU (flush stale I-cache entries):
   for (addr = start; addr < end; addr += CACHE_LINE_SIZE)
       asm("ic ivau, %0" :: "r"(addr) : "memory");
   asm("dsb ish" ::: "memory");   // Ensure all invalidates complete
   asm("isb" ::: "memory");        // Flush CPU pipeline (instruction fetch)

4. Now CPU can execute the new code at jit_code_buf.

Linux kernel provides:
  flush_icache_range(start, end) → does exactly the above
  // arch/arm64/include/asm/cacheflush.h

  flush_cache_all() → IC IALLUIS → invalidate all I-caches (SMP)
  __flush_icache_all() → per-CPU IC IALLU
```

---

## 5. DMA Cache Maintenance at PoC

```
DMA cache flush sequence (non-coherent DMA engine):

Scenario A: CPU writes, DMA reads (DMA_TO_DEVICE):
  Buffer: CPU fills buffer with data (in WB D-cache, dirty)
  Step 1: DC CIVAC for buffer range → clean+invalidate to PoC (DRAM)
          CPU's dirty data now in DRAM
          DMA engine reads from DRAM → correct fresh data ✓
  
  Linux: dma_sync_single_for_device()
    → caches_clean_inval_pou_checked() [for PoU]
    → flush_dcache_area(ptr, size):
         for (addr = start; addr < end; addr += D_CACHE_LINE_SIZE)
             asm("dc civac, %0" :: "r"(addr) : "memory");
         dsb(ish);

Scenario B: DMA writes, CPU reads (DMA_FROM_DEVICE):
  DMA engine writes to DRAM (buffer allocated but CPU may have touched it)
  Step 1: DC IVAC for buffer range → invalidate CPU cache (evict old lines)
          CPU's next read → cache miss → fetches fresh DMA data from DRAM ✓
  
  CAUTION: Only use IVAC (not CIVAC) here if certain lines are NOT dirty.
  If CPU has dirty data in these lines: use CIVAC instead to avoid data loss.
  
  Linux: dma_sync_single_for_cpu()
    → invalidate_dcache_area(ptr, size):
         for (addr = start; addr < end; addr += D_CACHE_LINE_SIZE)
             asm("dc ivac, %0" :: "r"(addr) : "memory");
         dsb(ish);

Scenario C: DMA reads AND writes (DMA_BIDIRECTIONAL):
  Use CIVAC before and after DMA operation.
  Both clean (for CPU→DMA) and invalidate (for DMA→CPU) required.
```

---

## 6. PoU vs PoC: Summary

```
┌─────────────────────────────────────────────────────────────────┐
│ Operation         │ Point   │ Instruction │ Use case            │
├───────────────────┼─────────┼─────────────┼─────────────────────┤
│ JIT code sync     │ PoU     │ DC CVAU     │ Flush new code to L2│
│ (I/D coherency)   │         │ IC IVAU     │ Inval I-cache from  │
│                   │         │             │ L2/PoU              │
├───────────────────┼─────────┼─────────────┼─────────────────────┤
│ DMA: CPU→Device   │ PoC     │ DC CIVAC    │ Flush dirty CPU     │
│                   │         │             │ data to DRAM        │
├───────────────────┼─────────┼─────────────┼─────────────────────┤
│ DMA: Device→CPU   │ PoC     │ DC IVAC     │ Inval CPU cache     │
│                   │         │             │ before reading DMA  │
│                   │         │             │ result from DRAM    │
├───────────────────┼─────────┼─────────────┼─────────────────────┤
│ Module load       │ Both    │ DC CIVAC    │ Flush code to DRAM  │
│ (code page)       │         │ IC IALLU    │ Inval all I-caches  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 7. Interview Questions & Answers

**Q1: What is the difference between PoU and PoC, and when do you use each?**

**PoU (Point of Unification)** is the cache level where the instruction cache, data cache, and page table walkers for a single core see the same data — typically L2. Operations to PoU (`DC CVAU`, `IC IVAU`) are used when you need to sync the I-cache with the D-cache on the SAME core, e.g., after JIT compilation: flush new code bytes from D-cache to L2 (PoU), then invalidate I-cache from L2 (PoU), so the instruction fetch sees the new code.

**PoC (Point of Coherency)** is where ALL system observers (all CPUs, all DMA engines, all cache levels) see coherent data — typically the LLC/L3 or DRAM. Operations to PoC (`DC CIVAC`, `DC IVAC`, `DC CVAC`) propagate data to/from DRAM, used for DMA cache management: `DC CIVAC` before DMA read (flush CPU's dirty data to DRAM), `DC IVAC` after DMA write (invalidate CPU's stale cache before reading DMA result).

**Q2: Why must you flush D-cache to PoU BEFORE invalidating the I-cache when preparing JIT code?**

If you invalidate the I-cache before flushing D-cache to PoU, the new instruction bytes are still in L1 D-cache (dirty, not yet in L2). After I-cache invalidation, the I-cache is empty. The CPU fetches instructions: I-cache miss → fetches from L2 (PoU). But L2 still has the OLD instruction bytes (D-cache hasn't flushed yet). The CPU loads and executes OLD instructions! The correct order: (1) `DC CVAU` → flush new bytes from D-cache to L2 (PoU), so L2 has fresh code. (2) `DSB ISH` → ensure flush completes. (3) `IC IVAU` → invalidate I-cache. (4) `DSB ISH + ISB` → ensure invalidation and pipeline flush complete. Now the next instruction fetch loads from L2 and finds the new instructions.

---

## 8. Quick Reference

| Point | Location (typical) | Scope | Used for |
|---|---|---|---|
| PoU | L2 cache | Per core | JIT/SMC: D-cache → L2, I-cache ← L2 |
| PoC | LLC/L3 or DRAM | All system | DMA: flush/inval to/from DRAM |

| Instruction | Direction | Scope | Use |
|---|---|---|---|
| `DC CVAU` | Clean to PoU | Per core | JIT: push new code to L2 |
| `IC IVAU` | Inval to PoU | Per core | JIT: inval I-cache for fresh fetch |
| `DC CIVAC` | Clean+Inval to PoC | Global | DMA TO_DEVICE: flush dirty CPU data |
| `DC IVAC` | Inval to PoC | Global | DMA FROM_DEVICE: inval stale CPU cache |
| `DC CVAC` | Clean to PoC | Global | Persist data without evicting from cache |
