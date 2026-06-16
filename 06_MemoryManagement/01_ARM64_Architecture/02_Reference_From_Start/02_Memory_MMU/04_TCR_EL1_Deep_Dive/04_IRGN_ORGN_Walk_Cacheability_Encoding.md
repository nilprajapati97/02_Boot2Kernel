# IRGN / ORGN: Page Table Walk Cacheability Encoding

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Why Walk Cacheability Matters

During a page table walk (TLB miss), the hardware Memory Management Unit (MMU) reads page table entries from memory. The IRGN and ORGN fields in TCR_EL1 control whether these reads are cached:

```
Without caching (IRGN=ORGN=0, Non-Cacheable):
  Each TLB miss requires 4 DRAM reads (for 4-level table, 4KB granule)
  DRAM latency: ~80–200 cycles per access
  Total penalty per TLB miss: 320–800 cycles
  On SMP with 8 cores, this serializes DRAM bandwidth

With caching (IRGN=ORGN=1, WB RA WA, Linux default):
  Page table entries cached in L1/L2/LLC
  Cache hit latency: 4–15 cycles per access
  Total penalty per TLB miss (all cached): 16–60 cycles
  PTW Cache (see Cat 03 file 09) further reduces to near-zero for hot tables

Performance ratio: ~10–50× faster with cached walks
This is why Linux always configures IRGN=ORGN=WB RA WA.
```

---

## 2. IRGN Encoding (Inner Cache for Walk Accesses)

```
TCR_EL1.IRGN0 (bits[9:8])  — inner cacheability for TTBR0 walks
TCR_EL1.IRGN1 (bits[25:24]) — inner cacheability for TTBR1 walks

Encoding (same for IRGN0 and IRGN1):
  0b00 = Normal memory, Inner Non-Cacheable (NC)
  0b01 = Normal memory, Inner WB RA WA (Write-Back, Read-Allocate, Write-Allocate)
  0b10 = Normal memory, Inner WT RA  (Write-Through, Read-Allocate, no Write-Allocate)
  0b11 = Normal memory, Inner WB RA  (Write-Back, Read-Allocate, no Write-Allocate)

"Inner" = L1 cache (and in some implementations, L2 within the cluster)
```

---

## 3. ORGN Encoding (Outer Cache for Walk Accesses)

```
TCR_EL1.ORGN0 (bits[11:10]) — outer cacheability for TTBR0 walks
TCR_EL1.ORGN1 (bits[27:26]) — outer cacheability for TTBR1 walks

Encoding (identical to IRGN encoding):
  0b00 = Normal memory, Outer Non-Cacheable (NC)
  0b01 = Normal memory, Outer WB RA WA
  0b10 = Normal memory, Outer WT RA
  0b11 = Normal memory, Outer WB RA

"Outer" = L2/L3/LLC (outside individual CPU clusters)
```

---

## 4. Write-Back vs Write-Through vs Non-Cacheable Explained

```
Non-Cacheable (NC) — 0b00:
  Every page table read hits DRAM directly
  No write buffering
  Guaranteed ordering: all walks see latest DRAM contents
  Slowest option: ~200–800 cycles per TLB miss
  Use: Debug builds, early boot before caches initialized

Write-Through + Read-Allocate (WT RA) — 0b10:
  Reads fill cache lines (read-allocate)
  Writes go to both cache AND DRAM simultaneously
  Read hits: fast (~4–15 cycles)
  Write hits: medium (~4–15 cycles cache + queued DRAM write)
  Write bandwidth: limited (DRAM bandwidth consumed on every write)
  Issue: Hardware AF/DBM writes to page table entries hit DRAM directly
  → AF/DBM workloads slower than WB

Write-Back + Read-Allocate + Write-Allocate (WB RA WA) — 0b01:
  Reads fill cache on miss (read-allocate)
  Writes fill cache on miss (write-allocate — avoids partial line writes)
  Modified data written to DRAM only on eviction or flush
  Fastest for PTW: cache acts as a fast page table store
  Hardware AF/DBM sets bits in cached PTEs → no DRAM write until eviction
  → Maximum performance for modern workloads
  This is the Linux default: IRGN=ORGN=0b01

Write-Back + Read-Allocate (WB RA) — 0b11:
  Reads fill cache on miss
  Writes do NOT fill cache on miss (non-write-allocate)
  Miss on write → write directly to next level (evicts current line)
  Inferior to WB RA WA for page table writes (AF bit sets may bypass cache)
  Not used by Linux
```

---

## 5. Linux Default Configuration

```c
// arch/arm64/include/asm/pgtable-hwdef.h

#define TCR_IRGN_WBWA   ((UL(1) << 8) | (UL(1) << 24))
//                       IRGN0=0b01     IRGN1=0b01
//                       TTBR0 inner WB RA WA
//                                        TTBR1 inner WB RA WA

#define TCR_ORGN_WBWA   ((UL(1) << 10) | (UL(1) << 26))
//                       ORGN0=0b01     ORGN1=0b01
//                       TTBR0 outer WB RA WA
//                                         TTBR1 outer WB RA WA

// In head.S, TCR_EL1 assembly:
#define TCR_EL1_VALUE   (TCR_T0SZ(VA_BITS) | TCR_T1SZ(VA_BITS) | \
                         TCR_TG_FLAGS       |                      \
                         TCR_IRGN_WBWA      |                      \
                         TCR_ORGN_WBWA      |                      \
                         TCR_SHARED_INNER   |                      \
                         TCR_IPS_FROM_FEATURES)
```

---

## 6. Interaction with MAIR_EL1

```
Important: IRGN/ORGN describe how the PTW hardware accesses page table memory.
They are NOT the same as the page table entries' AttrIndx (MAIR_EL1 slot).

Page table entries themselves are stored in Normal memory.
The OS sets MAIR_EL1 to define what AttrIndx=0,1,...,7 means for mapped pages.
But the PTW hardware uses IRGN/ORGN for reading those table entries —
independent of whatever MAIR_EL1 says.

┌─────────────────────────────────────────────────────────────┐
│  PTW reads page table entry (a Normal WB WA memory access)  │
│  Controlled by: TCR_EL1.IRGN + ORGN                         │
├─────────────────────────────────────────────────────────────┤
│  Resulting data access uses AttrIndx in the PTE              │
│  Controlled by: MAIR_EL1 + AttrIndx from PTE                │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. SMP Coherency Requirement

```
On SMP systems (multiple cores), all CPUs must have a coherent view
of page table memory. This requires:

1. Page tables in Inner Shareable memory (SH0/SH1 = 0b11)
2. IRGN = ORGN = 0b01 (WB RA WA, cacheable)
3. Cache coherency protocol (MESI/MOESI) ensures all CPUs see updates

Why Non-Cacheable (IRGN=ORGN=0b00) alone would also be "coherent":
  NC bypasses caches → all CPUs read from DRAM → same data
  But NC is very slow and would serialise all PTW on DRAM bandwidth

Why Cacheable + Inner Shareable gives coherency:
  ARM coherency interconnect (CCI, CMN) snoops all L1/L2 caches
  Any CPU writing a page table entry (e.g., setting AF bit) through
  the coherent interconnect → all other CPUs see the updated value
  No separate DRAM write needed until eviction

Forbidden combination (UNPREDICTABLE):
  IRGN=WB (cacheable) + SH=0b00 (Non-Shareable)
  → CPU0 may cache PTE in L1 without coherency
  → CPU1 reads a stale PTE from its own cache
  → TLB is filled with wrong PA → silent memory corruption
```

---

## 8. Boot Sequence Consideration

```
During early boot (before MMU enabled), caches may be disabled.
After MMU enable, the first page table walk uses IRGN/ORGN to
determine how to cache PTE reads.

Critical order in arch/arm64/kernel/head.S:
  1. Set MAIR_EL1 (memory attribute encoding)
  2. Set TCR_EL1 (including IRGN/ORGN)
  3. Set TTBR0_EL1 (page table base address)
  4. Set TTBR1_EL1 (kernel page table base)
  5. ISB (instruction sync barrier — ensures MSR changes take effect)
  6. Enable MMU: write SCTLR_EL1.M = 1
  7. ISB (flush pipeline after MMU enable)

If IRGN/ORGN are set AFTER MMU is enabled, the first few page table
walks may use the reset value (typically 0b00 = Non-Cacheable).
This is harmless but slow; most implementations set TCR before MMU enable.
```

---

## 9. Interview Questions & Answers

**Q1: What do IRGN and ORGN control, and what values does Linux use?**

IRGN (Inner Region) and ORGN (Outer Region) control the **cache attributes for page table walk memory accesses**. `IRGN` applies to the inner (L1) cache; `ORGN` applies to the outer (L2/L3/LLC) cache. The encoding is: `0b00`=Non-Cacheable, `0b01`=Write-Back Read-Allocate Write-Allocate, `0b10`=Write-Through Read-Allocate, `0b11`=Write-Back Read-Allocate. Linux uses `IRGN=ORGN=0b01` (WB RA WA) for both TTBR0 and TTBR1. This ensures page table entries are cached in both L1 and L2, giving the fastest possible TLB miss handling (~16–60 cycles total vs ~320–800 cycles for Non-Cacheable).

**Q2: Why is setting IRGN=0b01 (cacheable) but SH=0b00 (Non-Shareable) a dangerous combination?**

With `SH=Non-Shareable`, each CPU's cache operates independently without coherency snooping. If CPU0 modifies a page table entry (e.g., sets AF=1), the modified PTE remains in CPU0's L1 cache without being broadcast to CPU1. CPU1's subsequent page table walk (or TLB refill) reads either a stale cached value or the old DRAM value, filling its TLB with an incorrect translation. This leads to silent memory corruption where CPU1 accesses the wrong physical memory. The ARM spec marks this combination as UNPREDICTABLE. This is why Linux always pairs cacheable IRGN/ORGN with Inner Shareable (`SH=0b11`).

---

## 10. Quick Reference

| Field | Bits | What it controls |
|---|---|---|
| IRGN0 | [9:8] | Inner cache for TTBR0 PTW |
| ORGN0 | [11:10] | Outer cache for TTBR0 PTW |
| IRGN1 | [25:24] | Inner cache for TTBR1 PTW |
| ORGN1 | [27:26] | Outer cache for TTBR1 PTW |

| Encoding | Meaning | Linux uses? |
|---|---|---|
| 0b00 | Non-Cacheable | No (debug only) |
| 0b01 | WB Read-Alloc Write-Alloc | **YES (default)** |
| 0b10 | Write-Through Read-Alloc | No |
| 0b11 | WB Read-Alloc (no WA) | No |
