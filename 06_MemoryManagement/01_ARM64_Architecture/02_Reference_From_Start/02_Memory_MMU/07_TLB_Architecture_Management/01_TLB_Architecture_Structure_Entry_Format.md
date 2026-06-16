# TLB Architecture: Structure, Entry Format, and Microarchitecture

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. What is a TLB?

```
TLB (Translation Lookaside Buffer):
  A hardware cache for virtual-to-physical address translations.
  
  Without TLB:
    Every memory access requires 4 DRAM reads (for 4-level page tables):
      L0 table + L1 table + L2 table + L3 PTE → total: 4 extra DRAM accesses
      DRAM access ≈ 100–300 cycles
      4 × 100 = 400 extra cycles PER MEMORY ACCESS
      → Makes every memory access 400 cycles slower → unusable
  
  With TLB (cache of recent translations):
    Most virtual→physical translations are found in TLB: 1–4 cycles
    TLB miss: hardware page table walker (PTW) reloads TLB:
      ARM64 hardware walks page tables automatically → PTW fills TLB
      Software doesn't see the miss (unlike MIPS)
      After fill: TLB hit on retry → fast

  ARM64 TLB properties:
    Fully hardware-managed (HW PTW fills TLB on miss)
    No software-managed TLB miss handler required
    TLBI instructions needed only for explicit invalidation
```

---

## 2. TLB Hierarchy in ARM64

```
Typical ARM Cortex-A/Neoverse TLB hierarchy:

Per core (private):
  L1 iTLB (Instruction TLB):
    Separate TLB for instruction fetches
    Smaller: 32–64 entries (Cortex-A55), 48–96 (Cortex-X2)
    Fully associative (for small sizes) or set-associative
    Latency: 1 cycle
    
  L1 dTLB (Data TLB):
    Separate TLB for data load/store accesses
    Larger: 32–64 entries (Cortex-A55), 48–96 (Cortex-X2)
    Fully associative or set-associative
    Latency: 1 cycle

  L2 TLB (Unified):
    Shared by instruction and data (after L1 miss)
    Much larger: 512–4096 entries
    Set-associative (e.g., 4-way or 8-way)
    Latency: 4–8 cycles
    Holds translations for both iTLB and dTLB
    On miss: hardware PTW walks page tables

Cluster level (may exist on big.LITTLE, DSU clusters):
  L3 TLB (Cluster TLB): some implementations (rare)
  Mainly L2 TLB serves as last TLB level per core
  
Page Table Walker:
  Part of the MMU hardware (not a software routine for ARM64)
  On L2 TLB miss: PTW reads page table from DRAM/L1/L2 cache
  PTW is separate from the CPU pipeline — parallel walks possible
  PTW caches: Page Table Entry Caches (micro-TLBs or PTW caches)
    Some ARM implementations cache intermediate translations (L0/L1/L2 tables)
    to speed up repeated walks through the same table entries
```

---

## 3. TLB Entry Contents

```
A single TLB entry stores:

Tag (used for TLB lookup):
  Virtual Page Number (VPN): high bits of the VA
    For 4KB granule, 48-bit VA: VPN = VA[47:12] = 36 bits
  ASID (Address Space ID): 
    8-bit or 16-bit (from TTBR.ASID[63:48])
    Used to distinguish between different processes' translations
    ASID=0 is used for global mappings (nG=0 in PTE)
  VMID (Virtual Machine ID):
    8-bit or 16-bit, used when EL2 Stage 2 is active
    Distinguishes between different VMs' translations

Data (returned on TLB hit):
  Physical Page Number (PPN): PA[51:12] (40 bits for 52-bit PA)
  Memory attributes:
    AttrIndx[2:0] → looked up in MAIR_EL1 → cached attr in TLB entry
    SH[1:0] (Shareability)
    AP[2:1] (Access Permissions)
    PXN (Privileged Execute Never)
    UXN (Unprivileged Execute Never)
    nG (not Global — whether this entry is ASID-tagged)
    AF (Access Flag — copy of PTE AF bit)
    Contiguous hint (TLB may store contiguous hint for performance)
  Entry size:
    4KB, 16KB, 64KB, 2MB (block), 1GB (block)
    Larger entries cover more VA range per TLB entry

TLB entry size estimate:
  VPN: 36 bits (for 4KB, 48-bit VA)
  ASID: 16 bits
  PPN: 40 bits (52-bit PA)
  Attributes: ~15 bits (SH, AP, PXN, UXN, AttrIndx, AF, nG, size)
  Valid bit: 1 bit
  Total: ~108 bits ≈ 14 bytes per entry
  
  1024-entry TLB: ~14 KB of on-chip SRAM for the TLB
```

---

## 4. Fully Associative vs Set-Associative

```
Fully Associative TLB:
  Any entry can be placed in any slot
  Lookup: compare VA against ALL entries simultaneously (parallel comparison)
  Best hit rate (no conflict misses)
  Hardware cost: scales as O(N) — each entry needs a comparator
  Practical limit: 64–128 entries before area/power becomes too high
  
  Used for: L1 iTLB and L1 dTLB (small, 32–64 entries, FA is feasible)

Set-Associative TLB:
  N-way set associative: VA selects a "set" of N entries, compare those N
  Similar to set-associative caches
  Lower hardware cost (fewer comparators), can support many more entries
  Trade-off: conflict misses if many translations map to same set
  
  Used for: L2 TLB (512–4096 entries, FA impractical at this scale)
  Typical: 4-way or 8-way set-associative for L2 TLB

Replacement policy:
  LRU (Least Recently Used): theoretical ideal, expensive in hardware
  Pseudo-LRU: approximation of LRU using tree bits
  PLRU: Pseudo-random + LRU hybrid
  ARM implementations: typically pseudo-LRU or random replacement
  Programmer has no control over TLB replacement policy
```

---

## 5. TLB Sizes for Common ARM CPUs

```
Cortex-A55 (efficiency core, ARMv8.2):
  L1 iTLB: 32 entries fully associative
  L1 dTLB: 32 entries fully associative
  L2 TLB: 512 entries unified, 4-way set associative

Cortex-A78 (performance core, ARMv8.2):
  L1 iTLB: 48 entries
  L1 dTLB: 32 entries
  L2 TLB: 1024 entries 4-way

Cortex-X2 (premium performance, ARMv9):
  L1 iTLB: 64 entries
  L1 dTLB: 64 entries  
  L2 TLB: 2048 entries

ARM Neoverse N2 (server, ARMv9):
  L1 iTLB: 128 entries
  L1 dTLB: 64 entries
  L2 TLB: 4096 entries

Qualcomm Kryo (Snapdragon SoC):
  Similar to corresponding ARM big/little designs
  Customized sizes and associativity, exact numbers NDA'd
```

---

## 6. Interview Questions & Answers

**Q1: Why does ARM64 have separate L1 iTLB and dTLB instead of a unified L1 TLB?**

ARM64 CPUs use the Harvard architecture at L1: separate instruction and data caches. Likewise, separate L1 iTLB and dTLB allow simultaneous instruction fetch and data load/store to perform TLB lookups in the same cycle, without port conflicts. A unified TLB would need two read ports (for I-fetch and D-access simultaneously), doubling the area and power of the TLB SRAM. Separation also allows different sizes and organizations optimized for each use case: instruction access patterns (sequential fetch, predictable) differ from data access patterns (irregular, strided). The downside is that the same translation might be stored in both iTLB and dTLB, but this is a small overhead for large performance gain.

**Q2: What is in a TLB entry beyond just the VA-to-PA mapping?**

A TLB entry contains: (1) **Tag**: Virtual Page Number (VPN), ASID (for process isolation), and optionally VMID (for VM isolation). (2) **Physical Page Number (PPN)**: the translated PA. (3) **Memory attributes**: `AttrIndx`→memory type (Normal/Device/NC), `SH` (shareability), `AP[2:1]` (read/write permissions), `PXN`/`UXN` (execute-never bits), `nG` (ASID-tagged or global), `AF` (access flag). (4) **Entry size**: the size of the region this entry covers (4KB, 2MB, 1GB block). The attributes are cached in the TLB so that every memory access doesn't require going back to the page table to check permissions — the TLB hit provides both the PA AND all the access control information needed to complete the access.

---

## 7. Quick Reference

| Level | Type | Size (typical) | Assoc | Latency |
|---|---|---|---|---|
| L1 iTLB | I-fetch only | 32–128 entries | Fully | 1 cycle |
| L1 dTLB | Load/store | 32–64 entries | Fully | 1 cycle |
| L2 TLB | Unified | 512–4096 entries | 4–8 way | 4–8 cycles |
| PTW | HW walker | N/A | N/A | ~100–400 cycles (cache/DRAM) |

| TLB entry field | Size | Purpose |
|---|---|---|
| VPN | 36 bits (4KB, 48-bit VA) | Match virtual page |
| ASID | 8 or 16 bits | Process isolation |
| PPN | 40 bits (52-bit PA) | Translation result |
| Memory type | ~15 bits | Cache/access policy |
| Entry size | 3 bits | 4KB/16KB/64KB/2MB/1GB |
