# TLB Miss, Hardware Page Table Walker, and TLB Fill

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. TLB Miss Flow

```
TLB Miss: VA has no matching entry in L1 TLB + L2 TLB

Step 1: L1 TLB Lookup
  CPU loads from VA → L1 dTLB parallel lookup
  Compare VA[47:12]+ASID against all L1 dTLB entries
  Miss → forward to L2 TLB

Step 2: L2 TLB Lookup
  Set-associative lookup in L2 TLB
  VA selects TLB set → compare against set entries
  Miss → trigger hardware page table walker (PTW)

Step 3: Hardware PTW Walk
  PTW reads TTBR0_EL1 (or TTBR1_EL1 for kernel VA) for the root table PA
  PTW performs a 4-level table walk (for 4KB granule, 48-bit VA):
  
  L0 (PGD): PTW reads PA = TTBR0[47:12]|VA[47:39]<<3
             This is an 8-byte DRAM read (or cache hit)
             Reads L0 descriptor → extract L1 table PA
             
  L1 (PUD): PTW reads L1 descriptor at L0_PA|VA[38:30]<<3
             Extract L2 table PA
             
  L2 (PMD): PTW reads L2 descriptor at L1_PA|VA[29:21]<<3
             If this is a BLOCK descriptor (bit[1]=0, bit[0]=1):
               Translation complete: PA[47:21] from descriptor, PA[20:0]=VA[20:0]
             If TABLE descriptor: continue to L3
             
  L3 (PTE): PTW reads L3 descriptor at L2_PA|VA[20:12]<<3
             PAGE descriptor (bit[1]=1, bit[0]=1)
             PA = descriptor[47:12] | VA[11:0]

Step 4: TLB Fill
  PTW extracts PA + attributes from the leaf descriptor
  Inserts new entry into L2 TLB (and optionally L1 TLB)
  CPU resumes the stalled memory access with new TLB entry
  
  Hardware decides where to place the new entry (replacement policy)
  Software has no visibility into or control over TLB fill details
```

---

## 2. PTW Caching: Walk Cache

```
The hardware PTW accesses intermediate page table entries (L0/L1/L2 tables).
These are regular DRAM reads — subject to cache behavior.

Walk Cache (Translation Cache):
  ARM implementations typically cache the RESULTS of partial walks.
  
  "Intermediate Physical Address Cache" or "Table Walk Cache":
    Caches {VA_high_bits → intermediate table PA} mappings
    On a subsequent miss with the same high-order VA bits:
    PTW can skip the top table levels and start from the cached intermediate level
    
  This significantly speeds up TLB misses for processes with similar VA ranges.

Relationship to L1/L2 Data Cache:
  L0/L1/L2 table reads by PTW go through the DATA cache hierarchy.
  If the page table is in L1/L2 cache: PTW reads from cache (4–30 cycles) rather than DRAM (100+ cycles).
  
  This is why TCR_EL1 IRGN/ORGN settings matter:
    IRGN=0b01, ORGN=0b01 (WB RA WA): page tables cached → fast walk
    IRGN=0b00, ORGN=0b00 (NC): page tables bypass cache → slow walk
    Linux uses WB RA WA for maximum walk caching

PTW and cache coherency:
  If kernel modifies a page table entry, the CPU cache may hold the old PTE.
  PTW reads PTE from cache → still gets old entry → WRONG TRANSLATION!
  
  Fix: after modifying a PTE, execute TLBI to ensure:
    (a) Old TLB entry is invalidated
    (b) Any cached page table entries (walk cache) are also invalidated
    TLBI automatically invalidates walk cache entries too.
```

---

## 3. TLB Contention and Performance

```
TLB Miss Costs (typical ARM Cortex-A78/Neoverse N2):
  L1 TLB hit: 1 cycle
  L2 TLB hit: 4–8 cycles
  L2 TLB miss → PTW, all tables in L1 cache: 30–60 cycles
  L2 TLB miss → PTW, some tables in L2 cache: 60–120 cycles
  L2 TLB miss → PTW, tables in DRAM (cold): 400–800 cycles

TLB reach:
  4KB pages, 1024-entry L2 TLB: 1024 × 4KB = 4 MB of addressable memory
  If working set > 4 MB: TLB thrashing → every access is a TLB miss!
  
  Solution: huge pages
    2MB pages, 1024-entry TLB: 1024 × 2MB = 2 GB addressable → 512× better coverage
    This is why THP (Transparent Huge Pages) dramatically reduces TLB pressure
    for memory-intensive workloads.

TLB reach calculation:
  TLB reach = (TLB entries) × (page size)
  For 2MB huge pages: L2 TLB with 512 entries covers 512 × 2MB = 1 GB
  Even a small TLB with 512 entries covers 1GB when using 2MB pages

TLB conflict misses (set-associative TLB):
  If too many processes map to the same TLB set → eviction of valid entries
  Address layout can cause "TLB aliasing": multiple hot pages share a TLB set
  ARM64 TLB sets indexed by VA[12+set_bits:12] — spreading VA allocation helps
```

---

## 4. Contiguous Hint Bit and TLB Optimization

```
PTE bit[52] = Contiguous Hint:
  Tells hardware: this PTE is part of a contiguous set of aligned PTEs
  and can be merged into a single TLB entry covering all of them.

Contiguous ranges:
  4KB granule: 16 consecutive PTEs → 16 × 4KB = 64KB as ONE TLB entry
  64KB granule: 32 consecutive PTEs → 32 × 64KB = 2MB as ONE TLB entry
  2MB block: 32 consecutive 2MB blocks → 64MB as ONE TLB entry (ARM Cortex specific)

Effect:
  Instead of 16 TLB entries for 16 × 4KB pages:
  Hardware uses 1 TLB entry for the entire 64KB range → 16× better TLB efficiency
  
Requirement:
  All 16 (or 32) PTEs in the contiguous set must:
    - Be contiguous in VA (aligned to 64KB boundary for 4KB pages)
    - Map contiguous PA range (aligned to 64KB)
    - Have identical attributes (AP, SH, AttrIndx, PXN, UXN, AF)
    - All have Contiguous=1
    
  If any constraint is violated: hardware ignores the contiguous hint
  → Normal TLB entries (16 separate entries instead of 1)

Linux usage:
  huge_pte_mkcontig() sets Contiguous hint for hugepage mappings
  Used for HugeTLB and THP (when backed by 2MB blocks)
  mmap with large anonymous regions may get contiguous hint optimization
  
  ARM64_HW_CONT_PTE_SHIFT = 4 (log2 of 16 pages)
```

---

## 5. CnP Bit: Common Not Private (ARMv8.2)

```
TTBR.CnP bit (Common not Private):
  Bit[0] of TTBR0_EL1 and TTBR1_EL1

CnP=1:
  Declares that ALL CPUs sharing this translation regime are using
  IDENTICAL page table settings.
  
  Hardware implication:
    TLB entries for this TTBR can be SHARED between CPUs.
    Some implementations use a shared "outer" TLB level.
    Reduces TLB invalidation scope (one TLBI can serve multiple CPUs).
    
CnP=0 (default, before ARMv8.2):
  Each CPU maintains PRIVATE TLB entries for TTBR0.
  A TLBI IS (Inner Shareable) is still needed to invalidate across all CPUs.
  
Linux CnP usage (arch/arm64/mm/tlb.c):
  TTBR1_EL1 (kernel) always uses CnP=1 after SMP init:
    All CPUs share the same kernel page tables → CnP=1 safe
    Kernel TLB entries can be shared between CPUs → reduces kernel TLB miss overhead
    
  TTBR0_EL1 (user process):
    CnP=0 per-CPU (each process runs on one CPU at a time)
    On pinned kernel threads: CnP=1 possible if same mm on multiple CPUs
    
  Enabling CnP:
    cpu_set_reserved_ttbr0() → sets up CnP in kernel TTBR1
    FEAT_CSV2 (cache speculation restriction) interacts with CnP
```

---

## 6. Interview Questions & Answers

**Q1: Explain exactly what happens from the moment a TLB miss is detected to when the CPU can continue execution. What are the possible sources of latency?**

When a TLB miss is detected (entry not found in L1 iTLB/dTLB AND L2 TLB), the hardware Page Table Walker (PTW) takes over automatically — no software intervention. The PTW reads the appropriate TTBR (TTBR0 for user VA, TTBR1 for kernel VA), extracts the root table's physical address, then performs a 4-level walk for 4KB pages (L0→L1→L2→L3, or stops at L1/L2 for block descriptors). Each level read is a memory access that goes through the normal cache hierarchy (L1 → L2 → L3 → DRAM).

**Sources of latency**:
1. **All tables in L1 cache**: ~4 reads × 4 cycles = ~16 cycles total walk time
2. **L1 miss, L2 cache hit**: ~4 reads × 10 cycles = ~40 cycles
3. **L2 miss, DRAM**: ~4 reads × 100 cycles = ~400 cycles
4. **Walk cache hit** (hardware caches partial results): skip upper levels, ~1–2 DRAM reads
5. **PTW parallelism**: some CPUs support multiple outstanding PTW walks (for OOO instruction execution), hiding latency

After the walk, the CPU fills the TLB with the new entry and replays the original memory access (now a TLB hit). The total stall is the PTW walk time.

---

## 7. Quick Reference

| Walk level | VA bits used | Table entry size | Miss cost (DRAM) |
|---|---|---|---|
| L0 (PGD) | [47:39] | 8 bytes | ~100 cycles |
| L1 (PUD) | [38:30] | 8 bytes | ~100 cycles |
| L2 (PMD) | [29:21] | 8 bytes | ~100 cycles |
| L3 (PTE) | [20:12] | 8 bytes | ~100 cycles |
| **Total TLB miss (DRAM)** | | | ~400 cycles |

| Page size | TLB coverage (1024 entries) | Walk levels |
|---|---|---|
| 4KB | 4 MB | 4 (L0→L1→L2→L3) |
| 2MB (block) | 2 GB | 3 (L0→L1→L2 block) |
| 1GB (block) | 1 TB | 2 (L0→L1 block) |
