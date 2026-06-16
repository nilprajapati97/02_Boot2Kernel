# Normal Memory Attributes: Write-Back, Write-Through, Non-Cacheable

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Normal Memory vs Device Memory

```
ARM64 defines two top-level memory categories:

Device Memory:
  - Used for peripheral registers and memory-mapped IO
  - Strict ordering: nGnRnE, nGnRE, nGRE, GRE
  - No speculative access permitted
  - Not subject to cache coherency

Normal Memory:
  - Used for RAM (DRAM, SRAM), software data structures, code
  - Subject to cache coherency
  - Speculative execution and prefetching permitted
  - Defined by inner cache + outer cache attributes
  - ARM64 requires Normal memory for all software/OS data

Key distinction:
  "Normal" ≠ "Non-Cacheable"
  Normal just means it's not Device. Normal can be cacheable or not.
  Normal NC (0x44) is Normal memory with no caching.
```

---

## 2. The Two-Level Cache Hierarchy in MAIR

```
For Normal memory, the 8-bit MAIR attribute is split:

  Bits[7:4] = Outer attributes (outer cache: L2/L3/LLC)
  Bits[3:0] = Inner attributes (inner cache: L1, sometimes L2 within cluster)

This allows different policies for different cache levels:
  Normal WB WA (fully cached):  Outer=0b1111, Inner=0b1111 → 0xFF
  Normal NC:                    Outer=0b0100, Inner=0b0100 → 0x44
  Normal WT RA WA:              Outer=0b1011, Inner=0b1011 → 0xBB (roughly)

Note: "Outer" and "Inner" are architectural terms.
  On a Cortex-A57: Inner = L1 D-cache, Outer = L2
  On a Neoverse N2 (with CMN-700): Inner = per-CPU L1+L2, Outer = shared LLC
  The exact mapping is implementation-defined.
```

---

## 3. Write-Back (WB): Conceptual Foundation

```
Write-Back (WB) caching policy:

On WRITE:
  Data written to cache only (dirty bit set in cache line).
  DRAM is NOT updated immediately.
  Cache line stays dirty until: evicted, explicitly flushed (DC CVAC), or
  replaced by another cache miss.

On READ (cache hit):
  Returns data from cache (fast, ~4 cycles for L1).

On READ (cache miss):
  Fetches entire cache line from DRAM (64 bytes typical).
  Cache line becomes clean/dirty depending on whether a write follows.

On EVICTION (cache line must be replaced):
  If dirty: write dirty line to DRAM first (write-back).
  If clean: discard silently.

Advantages:
  - Write bandwidth: multiple writes to same cache line → only one DRAM write
  - Read-after-write hits cache → fast
  - Outstanding writes buffered → CPU not stalled by DRAM latency on writes

Disadvantages:
  - Dirty data may be lost on power failure or hardware error
  - DMA engine reading DRAM sees stale data (DMA coherency problem)
  - Memory dumps don't capture dirty cache lines

Full write-back encoding in MAIR (WB RA WA):
  Inner = 0b1111 = Write-Back, Read-Allocate, Write-Allocate
  Outer = 0b1111 = same
  8-bit attribute = 0xFF
  Linux: MT_NORMAL → slot 4 → 0xFF
```

---

## 4. Write-Through (WT): Conceptual Foundation

```
Write-Through (WT) caching policy:

On WRITE:
  Data written to BOTH cache AND DRAM simultaneously.
  No dirty cache lines.
  DRAM is always up-to-date.

On READ (cache hit):
  Returns data from cache (fast).

On READ (cache miss):
  Fetches from DRAM, fills cache.

On EVICTION:
  Cache line is always clean → just discard (no write-back needed).

Advantages:
  - DRAM always consistent with cache → DMA reads DRAM → sees current data
  - No dirty data loss on crash/power failure
  - Simpler coherency protocol

Disadvantages:
  - Every write generates DRAM traffic (write bandwidth = same as Non-Cacheable writes)
  - Write bandwidth LIMITED by DRAM bandwidth, not cache bandwidth
  - Write-heavy workloads (hash tables, databases) much slower than WB

MAIR encoding (WT RA):
  Inner = 0b0101 = Write-Through, Read-Allocate
  Outer = 0b0101 = same
  8-bit = 0x55 (or 0xBB for WB outer + WT inner)
  
Linux MT_NORMAL_WT = 0xBB:
  Outer = 0b1011 = WB RA (outer cache = L3 uses write-back)
  Inner = 0b1011 = WB RA (inner = L1 uses write-back)
  Hmm... actually 0xBB = both outer and inner WB RA (non-write-allocate)
  The exact WT encoding used in Linux kernel varies — check arch-specific code.
```

---

## 5. Non-Cacheable Normal Memory

```
Non-Cacheable (NC) Normal memory:

On WRITE:
  Data bypasses cache and goes directly to DRAM.
  No cache line allocated on write (no write-allocate).

On READ:
  Data bypasses cache and comes directly from DRAM.
  No cache line allocated on read (no read-allocate).

Used for:
  - DMA coherent buffers (CPU writes, DMA reads):
    If mapped as cached, CPU writes stay in cache → DMA reads stale DRAM.
    If mapped as Normal NC, CPU writes go directly to DRAM → DMA reads fresh.
    
  - Uncached BIOS/EFI variables
  
  - Persistent memory (PMEM/NVDIMM) where cache pollution is unwanted
  
  - Shared memory with other processors that don't participate in coherency

MAIR encoding:
  Inner = 0b0100 = Normal memory, Non-Cacheable
  Outer = 0b0100 = Normal memory, Non-Cacheable
  8-bit attribute = 0x44
  Linux: MT_NORMAL_NC → slot 3 → 0x44

Normal NC vs Device nGnRnE:
  Both bypass cache, but differ critically:
  Normal NC allows: speculative reads, prefetching, access reordering
  Device nGnRnE: NO speculative reads, NO prefetching, STRICT ordering
  
  Use Normal NC for DMA buffers (speculation is OK, it's just DRAM).
  Use Device nGnRnE for hardware registers (speculation = reading device state early)
```

---

## 6. Read-Allocate vs Write-Allocate

```
Allocation policy controls what happens on a CACHE MISS:

Read-Allocate (RA):
  On a read miss: allocate a new cache line, fill from DRAM/next-level cache
  The fetched data is placed in cache for future hits
  Standard for all cacheable reads
  
Write-Allocate (WA):
  On a write miss: allocate a new cache line, fill from DRAM, then write
  The written data is placed in cache (dirty)
  Benefit: future reads to same cache line hit cache
  Cost: one extra DRAM read (to fill the cache line before writing)
  
Non-Write-Allocate (no WA):
  On a write miss: write directly to DRAM without allocating cache line
  Benefit: no DRAM read penalty for allocation
  Cost: future reads to same address are still cache misses

When to use WA:
  Workloads that READ after WRITE: WA ensures the read is a cache hit.
  General-purpose data: WA (0b1111 = WB RA WA = 0xFF) is Linux default.

When to avoid WA:
  Write-once data (streaming writes, log files): WA pollutes cache with
  data that will never be read again. Use no-WA or Normal NC for streaming.
  
  Example: video encoder writing output stream:
    Output bytes written sequentially, never read back
    WA allocates cache lines → evicts hot data → cache thrashing
    Better: map output buffer as WT no-WA or Normal NC
```

---

## 7. Linux Memory Type Selection Guide

```
Scenario                              │ Memory Type     │ MAIR slot/value
──────────────────────────────────────┼─────────────────┼─────────────────
Kernel data, stack, page tables       │ Normal WB RA WA  │ MT_NORMAL (0xFF)
Kernel code (text segment)            │ Normal WB RA WA  │ MT_NORMAL (0xFF)
User data, heap, anonymous mmap       │ Normal WB RA WA  │ MT_NORMAL (0xFF)
DMA coherent buffer (sw coherency)    │ Normal NC        │ MT_NORMAL_NC (0x44)
DMA coherent (hw coherent SoC)        │ Normal WB RA WA  │ MT_NORMAL (0xFF)
Hardware register MMIO                │ Device nGnRnE    │ MT_DEVICE_nGnRnE (0x00)
PCIe MMIO BAR                         │ Device nGnRnE    │ MT_DEVICE_nGnRnE (0x00)
Write-combining (GPU framebuffer)     │ Device GRE       │ MT_DEVICE_GRE (0x0C)
Video/streaming write buffer          │ Normal NC        │ MT_NORMAL_NC (0x44)
Persistent memory (NVDIMM)            │ Normal NC        │ MT_NORMAL_NC (0x44)
```

---

## 8. Cache Coherency: Why DRAM-Backed Normal Memory Works

```
For Normal WB memory on SMP ARM64:
  All CPUs share a coherent interconnect (CCI/CMN)
  MESI/MOESI protocol ensures all CPUs see same data
  One CPU's L1 write is propagated to other CPUs via cache snooping
  No explicit cache flushes needed between CPUs for Normal WB memory

  This is WHY TCR_EL1.IRGN=ORGN=WB and SH=Inner Shareable works:
  Page tables are Normal WB Inner Shareable memory
  PTW hardware reads PTEs → through coherent cache → correct value

For Normal NC memory:
  No caching → no coherency protocol needed
  All CPUs read/write directly to DRAM → naturally consistent
  BUT: no L1 caching → every access is a DRAM access → slow

For Device memory:
  No cache snooping (device registers are not in coherent memory space)
  Coherency is the device's responsibility (or use barriers before/after)
```

---

## 9. Interview Questions & Answers

**Q1: What is the difference between Write-Back and Write-Through, and when would you choose each?**

**Write-Back**: Writes go to cache only; DRAM updated on eviction or flush. Benefits: fast writes (no DRAM traffic per write), multiple writes to same line generate one DRAM write. Drawbacks: DMA coherency issues (DMA reads stale DRAM), data loss on crash. Use for: general-purpose code and data (Linux default, `MT_NORMAL` = 0xFF).

**Write-Through**: Writes go to both cache and DRAM simultaneously. Benefits: DRAM always consistent (DMA-safe without flush), no dirty data on crash. Drawbacks: every write consumes DRAM bandwidth (write-heavy workloads slow). Use for: write-through video frame buffers, specific DMA scenarios without HW coherency.

For most ARM64 workloads, **Write-Back WA RA** (`0xFF`) is optimal. Write-Through is rarely used in Linux ARM64 because hardware coherency (on modern SoCs) or explicit DMA cache management handles coherency more efficiently.

**Q2: Why does Linux use Normal Non-Cacheable for DMA coherent buffers rather than Device nGnRnE?**

DMA coherent buffers are DRAM, not device registers. The key difference: Device `nGnRnE` prevents speculative execution and prefetching — the CPU cannot speculatively read ahead in the buffer or let the hardware prefetcher pipeline reads. This severely limits sequential DMA buffer throughput. Normal NC allows speculation and prefetching (it's just uncached DRAM reads/writes), so the hardware prefetcher can pipeline reads from a receive buffer, improving throughput. Both bypass cache, so both give the DMA engine access to current DRAM values. But Normal NC is faster for CPU accesses because speculation is allowed.

---

## 10. Quick Reference

| Policy | MAIR nibble | Write goes to | Eviction writes | Cache miss |
|---|---|---|---|---|
| WB RA WA | 0b1111 | Cache | DRAM | Allocate + fill |
| WB RA | 0b1101 | Cache (hit) / DRAM (miss) | DRAM | Read-alloc only |
| WT RA | 0b0101 | Cache + DRAM | None (clean) | Read-alloc |
| NC | 0b0100 | DRAM | None | No allocation |
