# Write-Back vs Write-Through vs Non-Cacheable Memory Types

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Normal Memory Attribute Overview

```
ARM64 Normal memory supports three caching policies:
  1. Write-Back (WB): most common, highest performance
  2. Write-Through (WT): less common, specific use cases
  3. Non-Cacheable (NC): no caching, always accesses DRAM

These are configured per-page via MAIR_EL1 + PTE AttrIndx[2:0]

MAIR_EL1 Normal memory encoding (per slot):
  Bits[7:4] = Outer cache attributes
  Bits[3:0] = Inner cache attributes
  
  0b0000 = Non-Cacheable (NC)
  0b01xx = Write-Through (WT)
    0b0100 = WT, Non-Write-Allocate (NWA), Read-Allocate (RA)
    0b0110 = WT, Write-Allocate (WA), Read-Allocate
  0b11xx = Write-Back (WB)
    0b1100 = WB, Non-Write-Allocate, Read-Allocate  
    0b1111 = WB, Write-Allocate, Read-Allocate (most common)
    
  xx in Inner matches Outer for typical Linux config
  MAIR_EL1 typical Linux slot 0 = 0xFF = WB, WA, RA, Outer=Inner=WB
```

---

## 2. Write-Back (WB) Cache Policy

```
Write-Back policy (the default for all Normal memory in Linux):

On WRITE:
  CPU writes to the CACHE LINE (not directly to DRAM)
  Cache line becomes DIRTY (Modified state in MESI)
  DRAM is NOT updated immediately
  
  Write-Allocate (WA=1, most common):
    If the write misses the cache (line not present):
    → Allocate a NEW cache line for this address
    → Perform the write to the new cache line (dirty)
    → Future reads to this cache line hit cache
    
  Non-Write-Allocate (WA=0, rarely used for Normal):
    If the write misses the cache:
    → Write DIRECTLY to DRAM (bypass cache)
    → No cache line allocated
    → Next read to this address still misses cache (no allocation happened)
    
On READ:
  Read-Allocate (RA=1, always for WB):
    On cache miss: allocate a new cache line, fill from DRAM
    Future accesses hit the cache

On EVICTION:
  Dirty cache line evicted: write back to DRAM or next cache level
  Clean cache line evicted: just discard (DRAM already has the data)

Advantages:
  - DRAM writes are minimized (batched via eviction)
  - Captures write locality (repeated writes to same location: 1 DRAM write total)
  - Highest performance for most workloads

Disadvantages:
  - DRAM may have stale data (requires explicit flush for DMA)
  - On crash/power loss: dirty cache data may be lost
  
Linux usage: ALL normal memory: kernel, user pages, vmalloc, kmalloc
```

---

## 3. Write-Through (WT) Cache Policy

```
Write-Through policy:

On WRITE:
  CPU writes to BOTH the cache line AND DRAM simultaneously (or via write buffer)
  Cache line is NEVER dirty (DRAM is always up-to-date)
  
  Non-Write-Allocate (typical for WT):
    Write miss: write to DRAM only, no cache allocation
    Read miss: allocate cache line from DRAM
  
  Write-Allocate (WT-WA):
    Write miss: allocate AND write to both cache and DRAM
    Wastes bandwidth vs WB-WA

On READ:
  Cache hit: return from cache (fast)
  Cache miss: read from DRAM, allocate in cache

On EVICTION:
  Never needs write-back (DRAM always current)
  Just discard the cache line

Advantages:
  - DRAM always has current data (no flush needed for DMA or coherency)
  - Simpler coherency for non-coherent devices (DMA sees current DRAM)
  - No data loss on power failure (data immediately in DRAM)
  
Disadvantages:
  - Every write goes to DRAM: higher write bandwidth
  - Writes not buffered/combined: write amplification for localized writes
    (Writing 1 byte: must write entire cache line to DRAM)
  - Typically 2–5× worse write performance than WB for write-heavy workloads

Use cases for WT:
  - Framebuffers / display memory: GPU DMA reads from DRAM frequently;
    WT ensures GPU always sees latest CPU writes without explicit flushes
  - Shared memory regions with frequent non-CPU readers (if coherency protocol
    doesn't cover the reader)
  - Debugging: use WT to make memory contents visible in DRAM immediately
    (useful when debugging DMA issues: no need to flush cache)
```

---

## 4. Non-Cacheable (NC) Normal Memory

```
Non-Cacheable Normal memory:
  No caching at ANY level (L1, L2, L3)
  Every access goes directly to DRAM
  
MAIR encoding: Outer=0b0000, Inner=0b0000
  But STILL Normal memory (not Device memory!)
  Difference from Device memory:
    Device memory: access reordering PROHIBITED, speculative reads PROHIBITED
    NC Normal: access reordering ALLOWED, speculative reads ALLOWED
    NC Normal can be used for real memory locations (structs, buffers)
    Device must be used for MMIO (side-effecting registers)

Why NC Normal is different from Device:
  Device memory: load from same address TWICE → must generate TWO bus transactions
  NC Normal: load from same address TWICE → hardware MAY coalesce into one bus tx
  This matters for performance: NC Normal can be optimized by hardware; Device cannot

Use cases for NC Normal:
  1. Coherent DMA buffer (dma_alloc_coherent):
     On non-coherent ARM SoCs: map as NC Normal
     CPU writes: go directly to DRAM (no cache)
     DMA device reads: gets current data from DRAM without cache flush
     
  2. Firmware/boot code that hardware needs to read without caches being active
  
  3. Debugging: force all accesses to DRAM for cache behavior analysis
  
  4. NVDIMM (persistent memory): map as NC Normal when PoP (Point of Persistence) 
     is DRAM/NVDIMM; ensures writes reach persistent storage without cache residue

NC performance characteristics:
  Read latency: full DRAM latency (100–300 cycles)
  Write latency: full DRAM write (~50–200 cycles, with write buffers)
  Throughput: limited by DRAM bandwidth (typically 30–100 GB/s on ARM64 SoCs)
  
  Comparison (approximate, Cortex-A78):
    WB read (cache hit):  4–12 cycles
    WB read (miss, DRAM): 150–300 cycles
    NC read:              150–300 cycles (always DRAM)
    → NC memory is 10–75× slower than WB for cache-hot data
```

---

## 5. Linux MAIR Configuration for Each Policy

```
Linux MAIR_EL1 default configuration (arch/arm64/include/asm/memory.h):

  #define MAIR_ATTRIDX_NORMAL_WB  0   // Attr[0] = 0xFF = WB-WA, both inner+outer
  #define MAIR_ATTRIDX_DEVICE_nGnRE 1 // Attr[1] = 0x04 = Device nGnRE
  #define MAIR_ATTRIDX_NORMAL_NC  2   // Attr[2] = 0x44 = NC Normal
  #define MAIR_ATTRIDX_NORMAL_WT  3   // Attr[3] = 0xBB = WT, RA, NWA

  MAIR_EL1 value (hex):
    [7:0]   Attr0 = 0xFF (Normal WB-WA, inner+outer)
    [15:8]  Attr1 = 0x04 (Device nGnRE)
    [23:16] Attr2 = 0x44 (NC Normal, inner+outer)
    [31:24] Attr3 = 0xBB (WT RA NWA, inner+outer)
    [63:32] Attr4-7 = 0x00 (unused)
    
  MAIR_EL1 = 0x00000000_BB4404FF

PTE attribute index selection:
  Page table descriptor bits[4:2] = AttrIndx[2:0]:
    0b000 (0) → MAIR Attr0 → Normal WB-WA
    0b001 (1) → MAIR Attr1 → Device nGnRE
    0b010 (2) → MAIR Attr2 → NC Normal
    0b011 (3) → MAIR Attr3 → WT
    
  Linux page flags:
    PAGE_KERNEL      = AttrIndx=0 = WB (default kernel RW)
    PAGE_KERNEL_EXEC = AttrIndx=0 = WB (kernel executable)
    PROT_NORMAL      = AttrIndx=0 = WB (user pages)
    PROT_NORMAL_NC   = AttrIndx=2 = NC (coherent DMA)
    PROT_DEVICE_nGnRE = AttrIndx=1 = Device nGnRE (MMIO)
    
  pgprot_writecombine():
    Returns PROT_DEVICE_nGnRE or PROT_NORMAL_NC depending on platform
    Used for: framebuffer mapping, GPU memory (write-combining)
    
  pgprot_noncached():
    Returns PROT_DEVICE_nGnRnE (strongest barrier — no reordering)
    Used for: critical MMIO (DMA control registers, PCI config space)
```

---

## 6. Interview Questions & Answers

**Q1: Why does a DMA coherent buffer use NC Normal memory rather than Device memory?**

`dma_alloc_coherent()` returns memory that BOTH the CPU and DMA device can access safely. It uses **NC (Non-Cacheable) Normal memory** rather than Device memory for a critical reason: the CPU may perform **speculative reads** and **reorder accesses** on Normal memory — this is harmless for regular data structures. But **Device memory** prohibits speculative reads and access reordering, treating every access as a side-effecting hardware register. If CPU code (e.g., a driver) does `ptr[0] + ptr[1]` with Device-typed memory, the CPU cannot optimize or speculate these reads — every access is a serialized, non-speculative bus transaction.

For a DMA coherent buffer (a regular data buffer shared with a DMA engine), we WANT the CPU to be able to speculatively prefetch and optimize accesses — the data is regular structured data, not hardware registers. NC Normal gives us: (a) no caching (data always in DRAM, DMA device sees fresh data without cache flushes), AND (b) Normal memory access semantics (speculative reads allowed, load/store reordering allowed, hardware write buffer coalescing allowed). This gives much better CPU read performance for the coherent buffer compared to Device memory typing.

---

## 7. Quick Reference

| Policy | On Write | DRAM Current? | Write Bandwidth | Eviction Writeback |
|---|---|---|---|---|
| Write-Back (WB) | Cache only | No (dirty) | Low (cached) | Yes (when evicted) |
| Write-Through (WT) | Cache + DRAM | Yes (always) | High (every write) | No |
| Non-Cacheable (NC) | DRAM directly | Yes (always) | Medium | N/A (no cache) |

| MAIR Encoding | Policy | Linux Usage |
|---|---|---|
| 0xFF | WB-WA, Inner+Outer | Default: all kernel + user pages |
| 0xBB | WT-RA-NWA, Inner+Outer | Framebuffer (some platforms) |
| 0x44 | NC Normal, Inner+Outer | Coherent DMA, dma_alloc_coherent |
| 0x04 | Device nGnRE | MMIO registers |
| 0x00 | Device nGnRnE | Critical MMIO (PCIe config) |
