# Cache Coherency: MESI, MOESI, and ARM64 Coherent Interconnect

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Why Cache Coherency Is Needed

```
Multi-core system problem:
  Core 0 reads variable X=5 from DRAM → stores in Core 0's L1 cache
  Core 1 reads variable X=5 from DRAM → stores in Core 1's L1 cache
  Core 0 writes X=10 → updates Core 0's L1 cache
  Core 1 reads X → reads from its own L1 cache → sees X=5 (WRONG!)
  
  Without coherency: each core sees a different value of X
  → Data races, incorrect computation, memory corruption
  
Cache coherency protocol: hardware mechanism ensuring ALL caches see
a consistent view of memory. The fundamental invariant:
  
  "At any point in time, if any cache holds a copy of a memory location,
   all cached copies must agree on the current value."
   
  (Formal: SWSS — Single Writer Multiple Reader invariant)
```

---

## 2. MESI Protocol

```
MESI states (4 states per cache line):

M (Modified):
  This cache has the ONLY copy; it has been WRITTEN (differs from DRAM)
  No other cache has this line
  DRAM copy is STALE (our modified value not written back yet)
  On eviction: MUST write back to DRAM (write-back policy)

E (Exclusive):
  This cache has the ONLY copy; it is CLEAN (matches DRAM)
  No other cache has this line
  Can be written without any bus transaction (transitions to M silently)
  Can be evicted without write-back (DRAM already has correct value)

S (Shared):
  MULTIPLE caches may have this line; all copies are CLEAN
  DRAM has the authoritative copy
  Cannot be written without invalidating other copies first
  
I (Invalid):
  This cache line does NOT contain valid data
  Any access → cache miss → fetch from next level

State transitions:

  Processor read:
    I → E: fetch from DRAM (no other cache has it) → Exclusive
    I → S: fetch from DRAM (another cache also has it) → Shared
    S → S: hit, no state change
    E → E: hit, no state change
    M → M: hit, no state change
    
  Processor write:
    I → M: fetch from DRAM + invalidate all other copies → Modified
    S → M: send Invalidate to all other caches with copy → Modified
    E → M: no bus transaction needed → write silently, M
    M → M: hit, no state change
    
  Bus snoop (other core requests):
    M → S: another core reads → we must flush to DRAM (write-back) → Shared
    M → I: another core writes → we must flush to DRAM → Invalid
    E → S: another core reads → transition to Shared
    E → I: another core writes → transition to Invalid
    S → I: another core writes → Invalidate (BusUpgr/RWITM)
```

---

## 3. MOESI Protocol Extension

```
MOESI adds the O (Owned) state to MESI:

O (Owned):
  This cache has a MODIFIED copy
  BUT: other caches may also have STALE SHARED copies
  DRAM is STALE (this core has the latest value)
  Responsibility: if another core reads this line, we SUPPLY the data
    (instead of going to DRAM, which has stale data)
  
  Key difference from M: in MOESI, when another core reads a Modified line,
  the owning core can respond directly (Cache-to-Cache transfer) WITHOUT
  first writing back to DRAM, and then transitions to O (Owned) while
  the requesting core gets S (Shared).
  
  In MESI: M → dirty eviction → DRAM write-back → other core reads from DRAM
  In MOESI: M → O (owner supplies data directly) → requester gets S
  Benefit: avoids DRAM round-trip for cache-to-cache sharing
  
  Common in: AMD processors, some ARM implementations

ARM AMBA ACE (AXI Coherency Extensions):
  ARM's coherency protocol for SoCs uses ACE
  Effectively implements MESI-equivalent for ARM CPUs
  
  ARM cache line states in ACE:
    UC (Unique Clean): like E — only copy, clean
    UD (Unique Dirty): like M — only copy, modified
    SC (Shared Clean): like S — multiple copies, clean
    SD (Shared Dirty): like O — shared but modified copy held here
    IN (Invalid): like I
    
  ACE transactions:
    ReadUnique: fetch line exclusively (I → UD, for write)
    ReadClean: fetch line shareable (I → SC, for read)
    MakeUnique: upgrade S→UD without fetching data (have the data, want exclusivity)
    CleanUnique: write back dirty data but stay clean and exclusive
    WriteBack: write M/UD line back to memory on eviction
```

---

## 4. ARM64 Coherent Interconnect

```
ARM CoreLink / CCI (Cache Coherent Interconnect) architecture:
  All CPUs in an ARM SoC connect via CCI or NIC
  
  CCI-550/CCI-700: handles coherency between CPU clusters
  NoC (Network on Chip): routes transactions between clusters, GPU, DMA

Snoop Filter:
  Hardware tracking which cache holds copies of each cache line
  Located in the interconnect (not per-cache)
  Avoids broadcasting every snoop to ALL caches:
    Without filter: "Does anyone have cache line X?" → broadcast to all cores
    With filter: snoop filter knows "Core 2 and Core 5 have X" → targeted snoop
  
  Benefit: reduces interconnect bandwidth for snoop traffic
  Trade-off: snoop filter itself uses area and power
  
  Cortex-A77/A78 cluster: 4-core cluster with shared L3 and snoop filter
  Snapdragon: 4-core performance cluster + 4-core efficiency cluster
              each with its own snoop domain, connected via NoC

Coherency domains:
  Inner Shareable domain: typically one CPU cluster + GPU (shares L2/L3)
  Outer Shareable domain: multiple clusters + all DMA-capable devices
  
  DMB ISH: barrier within Inner Shareable domain
  DMB OSH: barrier for Outer Shareable domain
  DMB SY: full system barrier
  
  DMA coherency:
    If DMA device is in the Outer Shareable domain: DMA sees CPU writes after DMB OSH
    If DMA device is NOT coherent (non-ACP DMA): must DC CIVAC + DSB before DMA read
```

---

## 5. Linux Cache Coherency Handling

```
Linux kernel assumptions for ARM64 SMP:
  CPUs are cache-coherent (ARM64 SMP requires Inner Shareable coherency)
  CPU-to-CPU data sharing: no explicit cache operations needed
  Ordering: memory barriers (DMB/DSB) handle ordering, not cache flushes
  
  This is unlike x86 where TSO (Total Store Order) provides implicit ordering
  ARM64 uses a weakly-ordered model → explicit barriers required

DMA coherency (non-coherent DMA):
  Most ARM SoC DMA engines: NOT cache-coherent
  DMA reads from PA → bypasses CPU L1/L2 caches → reads stale DRAM data
  
  Linux DMA API:
    dma_map_single(dev, va, size, DMA_TO_DEVICE):
      dc_cvac(va, size): clean D-cache to PoC (write dirty data to DRAM)
      dsb(ishst): ensure completion
      → DMA engine now sees latest CPU writes in DRAM
      
    dma_map_single(dev, va, size, DMA_FROM_DEVICE):
      dc_ivac(va, size): invalidate D-cache (discard stale CPU copies)
      → After DMA writes to DRAM, CPU will fetch fresh data on next access
      
    dma_map_single(dev, va, size, DMA_BIDIRECTIONAL):
      dc_civac(va, size): clean + invalidate (both directions)

Kernel SMP memory operations (no cache flushes needed):
  smp_store_release(p, v): STLR instruction (acquire/release semantics)
  smp_load_acquire(p): LDAR instruction
  These provide ordering without cache flushes (coherency handled by HW)
```

---

## 6. Interview Questions & Answers

**Q1: In MESI protocol, what happens when Core A (holding M state for cache line X) and Core B both try to write to X simultaneously?**

In MESI, only ONE core can hold a line in M (Modified) state at any time — this is the fundamental coherency invariant. When Core B wants to write to X while Core A holds it in M:

1. Core B issues a `BusUpgr` (Bus Upgrade) transaction — or `ReadWithIntent to Modify (RWITM)` if Core B doesn't have the line yet
2. The interconnect/coherency controller detects that Core A has the line in M state
3. Core A's cache controller receives a `Snoop` (or `Invalidate` signal)
4. Core A performs a "dirty writeback" — writes its modified data to the L3 cache or DRAM
5. Core A transitions its line to I (Invalid) state
6. Core B's request completes: it receives the data (either from DRAM or L3), transitions to M state, and performs its write

The two writes appear atomic from the coherency perspective: they cannot interleave — one must fully complete before the other begins. The hardware serializes them through the interconnect's arbitration. However, the **ordering** between Core A's and Core B's writes depends on the memory model and any software synchronization primitives — the hardware ensures coherency, not ordering beyond what the memory model specifies.

---

## 7. Quick Reference

| State | Copies | Dirty? | Write allowed? | DRAM valid? |
|---|---|---|---|---|
| M (Modified) | 1 | Yes | Yes (already exclusive) | No |
| E (Exclusive) | 1 | No | Yes (silent → M) | Yes |
| S (Shared) | Multiple | No | No (must invalidate others) | Yes |
| I (Invalid) | 0 | N/A | No (must fetch) | Yes |
| O (Owned, MOESI) | Multiple | Yes | No | No |

| ARM ACE State | MESI Equivalent |
|---|---|
| UC (Unique Clean) | E |
| UD (Unique Dirty) | M |
| SC (Shared Clean) | S |
| SD (Shared Dirty) | O |
| IN (Invalid) | I |
