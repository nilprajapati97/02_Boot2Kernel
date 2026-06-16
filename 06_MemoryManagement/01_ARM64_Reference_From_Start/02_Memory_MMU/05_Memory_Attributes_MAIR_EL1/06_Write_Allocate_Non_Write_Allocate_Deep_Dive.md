# Write-Allocate vs Non-Write-Allocate: Deep Dive

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. What Allocation Policy Controls

Cache allocation policy determines what happens when a cache **misses** (the requested data is not in the cache):

```
Two independent allocation decisions:
  1. READ-ALLOCATE (RA): Does a read miss allocate a cache line?
  2. WRITE-ALLOCATE (WA): Does a write miss allocate a cache line?

For each level (inner/outer), these can be set independently:
  Both RA + WA: allocate on miss for both reads and writes
  RA only: allocate for reads, not writes
  Neither: streaming mode — no allocation at all (= Non-Cacheable)
  WA only: allocate for writes only (unusual)
```

---

## 2. Read-Allocate (RA) Behavior

```
RA = 1 (Read-Allocate):
  On a read miss (data not in cache):
    1. Cache controller selects a victim line (LRU or pseudo-LRU)
    2. If victim is dirty: write victim back to DRAM first
    3. Fetch requested data from DRAM (full cache line, typically 64 bytes)
    4. Store in cache
    5. Return data to CPU

  Subsequent reads to same cache line → cache HIT (fast, ~4 cycles)
  
RA = 0 (No Read-Allocate):
  On a read miss:
    1. Fetch data from DRAM
    2. Return to CPU
    3. DO NOT store in cache
  
  No cache pollution (useful for streaming data read once)
  Subsequent reads to same address → cache MISS again

When to disable RA:
  Sequential scans reading large arrays once (video decode, checksum)
  Data read exactly once has no benefit from caching → no-RA prevents
  evicting frequently-used data ("cache trashing") from the cache.
```

---

## 3. Write-Allocate (WA) Behavior

```
WA = 1 (Write-Allocate):
  On a write miss (address not in cache):
    1. FIRST: fetch the cache line from DRAM ("read-for-ownership")
    2. Merge the write into the now-cached line
    3. Mark line dirty
    4. Return to CPU

  Why fetch first? Because we're only writing part of a cache line.
  A cache line is 64 bytes. Write is maybe 8 bytes.
  To write 8 bytes "correctly" in a WB cache, we need the other 56 bytes
  too (so they're preserved when the dirty line is eventually evicted).
  
  The initial fetch = "read-for-ownership" (RFO)
  
  Subsequent writes to SAME cache line → cache HIT (no RFO needed)
  Subsequent reads to written address → cache HIT

WA = 0 (No Write-Allocate):
  On a write miss:
    Write goes DIRECTLY to DRAM (write-through behavior for miss case)
    DO NOT allocate a cache line
  
  No cache pollution for write-only data
  No RFO overhead (no extra DRAM read)
  But: subsequent reads miss → RFO will happen then if RA=1

When to disable WA:
  Write-only data that will never be read back by this CPU:
    Log files, output streams, DMA output buffers
    Video encoder writing encoded bitstream to output buffer
    Cryptographic output (hash/encrypt → write to output, never read back)
```

---

## 4. MAIR Encoding: All Combinations

```
Normal memory inner/outer nibble encoding:

Bit pattern │ RA │ WA │ Policy
────────────┼────┼────┼────────────────────────────────────────
0b0000      │ -  │ -  │ Non-Cacheable (NC) — same as Device-like but Normal
0b0001      │ -  │ -  │ Reserved
0b0010      │ -  │ -  │ Reserved
0b0011      │ -  │ -  │ Reserved
0b0100      │ 0  │ 0  │ Normal NC (explicitly)
0b0101      │ 1  │ 0  │ Write-Through, Read-Allocate, No WA (WT RA)
0b0110      │ 1  │ 1  │ Write-Through, Read-Allocate, Write-Allocate (WT RA WA)*
0b0111      │ 1  │ 0  │ Write-Through, Read-Allocate (WT RA) [variation]
0b1000      │ 0  │ 0  │ Write-Back, Non-Allocate (WB no-alloc)
0b1001      │ 1  │ 0  │ Write-Back, Read-Allocate (WB RA)
0b1010      │ 0  │ 1  │ Write-Back, Write-Allocate (WB WA) [unusual]
0b1011      │ 1  │ 0  │ Write-Back, Read-Allocate (WB RA) [variation]
0b1100      │ 0  │ 0  │ Write-Back, Non-Allocate (WB)
0b1101      │ 1  │ 0  │ Write-Back, Read-Allocate (WB RA)
0b1110      │ 1  │ 1  │ Write-Back, Read+Write Allocate (WB RA WA)
0b1111      │ 1  │ 1  │ Write-Back, Read+Write Allocate (WB RA WA) ← Linux default

* WT WA is unusual: write-through to DRAM, but on miss, fetch first (RFO)
  Rarely useful in practice.

Linux uses 0b1111 (0xF per nibble) → full WB RA WA → 0xFF for both inner+outer
```

---

## 5. Performance Analysis: WA vs no-WA for Key Workloads

```
Workload: Random in-place updates (database B-tree, hash table)

Access pattern:
  Read key → modify value → write back to same address
  Same cache lines read and written multiple times

With WB RA WA (Linux default):
  First write miss: RFO (1 DRAM read) + write to cache line
  Subsequent writes to same line: CACHE HIT → no DRAM traffic
  First read miss (before write): fetch from DRAM
  Reads after write: CACHE HIT → fast

With WB RA (no WA):
  First write miss: write directly to DRAM (bypasses cache)
  Subsequent writes to same address: DRAM misses (data not in cache after writes)
  First read miss: fetch from DRAM
  Reads after write: read DRAM value (write went directly to DRAM, so DRAM is current)
  BUT: no cache benefit for write-heavy paths

Result: WA is ~2–5× faster for random write-heavy workloads
  (WA allows write coalescing in cache; no-WA hits DRAM on every write)

───────────────────────────────────────────────────────────

Workload: Sequential streaming writes (memcpy destination, file I/O write buffer)

Access pattern:
  Write 64MB sequentially, never read back

With WB RA WA:
  Each write miss: RFO (read 64 bytes), merge write, mark dirty
  Eviction: write 64 bytes to DRAM
  Net: 2 DRAM transactions per cache line (RFO read + dirty write)
  Cache pollution: 64MB of "never read" data evicts hot working set

With WB RA (no WA):
  Each write miss: write directly to DRAM (no RFO)
  Net: 1 DRAM transaction per write (write only, no read)
  No cache pollution: hot working set stays in cache

Result: no-WA is ~2× better for streaming writes (no RFO overhead)
  AND prevents cache thrashing

This is why DCMVAC (Clean to PoC) + no-WA is used for DMA output buffers.
```

---

## 6. Real-World Memory Attribute Tuning

```
Android / Mobile SoC:
  Camera DMA output: ioremap with WB RA WA (coherent SoC)
    Camera hardware writes, CPU reads → WB preserves locality for CPU processing

  Audio I/O buffer: Normal NC
    Real-time audio: CPU writes samples, audio DMA reads directly
    NC ensures DMA sees fresh samples without explicit flushes

High Performance Computing (NVIDIA GPU server, ARM Neoverse):
  Matrix data: Normal WB RA WA (0xFF)
    DGEMM repeatedly reads same matrix blocks → WB RA WA gives L3 cache hits
  
  Streaming input array (read once): Could use NC or no-RA
    But ARM64 Linux doesn't have per-mmap cache hint (unlike x86 NT stores)
    madvise(MADV_SEQUENTIAL) → readahead but no cache type change on ARM64
    For true streaming, use __builtin_prefetch or SVE streaming mode

  GPU output buffer: Normal NC or ioremap_wc (GRE)
    If SoC is not GPU-coherent: NC ensures CPU sees GPU output directly from DRAM
    If GPU-coherent (Arm Mali + CMN-700): WB RA WA is fine (GPU participates in snoop)
```

---

## 7. Interview Questions & Answers

**Q1: Explain Write-Allocate policy and when you would disable it.**

**Write-Allocate** means: on a write cache miss, the cache first fetches the full cache line from DRAM ("read for ownership"), merges the write into the fetched line, and marks it dirty. This is beneficial when the same cache line will be both written and subsequently read, because the data stays in cache for fast read-back. However, for **write-only streaming workloads** (e.g., a video encoder writing output bytes sequentially, never reading them back), WA is harmful: it generates an extra DRAM read (the RFO) for every write miss, and pollutes the cache with data that will never be re-read — evicting more useful data. In those cases, **no-WA** (write directly to DRAM without allocating a cache line) gives better performance. Linux uses WB RA WA (`0xFF`) as the default for general-purpose memory since most workloads benefit from it, but DMA output buffers and streaming writes are better served by NC or WB no-WA configurations.

---

## 8. Quick Reference

| Policy | Nibble | Read miss | Write miss | Use case |
|---|---|---|---|---|
| WB RA WA | 0b1111 (0xF) | Allocate + fill | RFO + allocate | General purpose (default) |
| WB RA | 0b1101 (0xD) | Allocate + fill | Write to DRAM | Read-heavy, write-once |
| WT RA | 0b0101 (0x5) | Allocate + fill | Write DRAM+cache | Write-through scenarios |
| NC | 0b0100 (0x4) | No alloc | No alloc | DMA, MMIO, streaming |
