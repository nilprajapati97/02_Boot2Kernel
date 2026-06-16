# SH0 / SH1: Shareability Domain for Page Table Walks

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. What is Shareability?

ARM64 defines memory "shareability" domains to control which agents (CPUs, accelerators, peripherals) are part of a coherence group for a given memory region. The SH fields in TCR_EL1 apply specifically to the **page table memory** read during hardware page table walks.

```
Shareability domains (ARM64 Architecture):

Non-Shareable (NSH):
  Only the local processor (or local cluster) observes coherency.
  Other CPUs may see stale data.

Outer Shareable (OSH):
  All agents within the outer shareability domain observe coherency.
  Typically includes all CPU clusters + some accelerators.
  L3/LLC cache is shared across all clusters.

Inner Shareable (ISH):
  All agents within the inner shareability domain observe coherency.
  Typically one CPU cluster (4–16 cores sharing L1/L2 + CCI/CMN interconnect).
  On most ARM64 SoCs, ISH = all CPUs on the SoC.
  This is because the interconnect covers all CPU clusters.

System (SY):
  Available in barrier instructions (DMB SY, DSB SY) but not in TCR.
  Includes DRAM controller, PCIe, DMA engines.
```

---

## 2. SH Encoding in TCR_EL1

```
SH0 (bits[13:12]) — shareability for TTBR0 page table walks
SH1 (bits[29:28]) — shareability for TTBR1 page table walks

Encoding (same for SH0 and SH1):
  0b00 = Non-Shareable
  0b01 = Reserved (UNPREDICTABLE — never use!)
  0b10 = Outer Shareable
  0b11 = Inner Shareable ← Linux default

Linux constant:
  #define TCR_SHARED_INNER  ((3UL << 12) | (3UL << 28))
  //                          SH0=0b11      SH1=0b11
  //                          = Inner Shareable for both TTBR0 and TTBR1
```

---

## 3. Why Inner Shareable is Required for SMP

```
SMP (Symmetric Multi-Processing) scenario:
  CPU0 maps a new page → writes PTE to page table memory (in L1 cache)
  CPU1 gets a TLB miss → hardware PTW reads PTE for same VA
  
  With SH=Non-Shareable:
    CPU0's PTE write stays in CPU0's L1 cache
    CPU1's PTW reads from CPU1's L1 cache (stale) or from DRAM (old value)
    CPU1 installs wrong translation in its TLB
    CPU1 accesses wrong physical memory → silent data corruption

  With SH=Inner Shareable + IRGN/ORGN cacheable:
    CPU0's PTE write goes through coherent interconnect (CCI/CMN)
    ARM coherency protocol (MESI) marks CPU1's cached copy as Invalid
    CPU1's PTW reads the updated PTE via snoop (fresh from CPU0's L1)
    CPU1 installs correct translation
    → No corruption, no explicit flush needed

Physical interconnect on ARM SoC:
  ┌────────────────────────────────────────────────────────────┐
  │                     CMN-600/CCI-500                        │
  │  (Coherent Mesh Network / Cache Coherent Interconnect)     │
  │                                                            │
  │  CPU Cluster 0           CPU Cluster 1                     │
  │  ┌──────────────┐        ┌──────────────┐                  │
  │  │ CPU0 L1 D$   │        │ CPU4 L1 D$   │                  │
  │  │ CPU1 L1 D$   │←snoop→ │ CPU5 L1 D$   │                  │
  │  │ CPU2 L1 D$   │        │ CPU6 L1 D$   │                  │
  │  │ CPU3 L1 D$   │        │ CPU7 L1 D$   │                  │
  │  │ Shared L2    │        │ Shared L2    │                  │
  │  └──────────────┘        └──────────────┘                  │
  │                                                            │
  │              Shared L3 (LLC) / DRAM                        │
  └────────────────────────────────────────────────────────────┘
  
  All CPUs in ISH domain → any L1/L2 write is snooped by all CPUs
  Page table entry written by one CPU immediately visible to all
```

---

## 4. The UNPREDICTABLE Combination

```
FORBIDDEN: Cacheable + Non-Shareable for Page Tables

  IRGN/ORGN = 0b01 (WB RA WA, cacheable)
  SH = 0b00 (Non-Shareable)

This combination is explicitly marked UNPREDICTABLE in ARM ARM (Architecture Reference Manual).

Why it's dangerous:
  - Cacheable data lives in CPU's private cache
  - Non-Shareable: no coherency protocol enforced
  - CPU0 writes PTE to L1 → stays in CPU0's local cache indefinitely
  - CPU1 reads PTE → cache miss → reads stale DRAM value
  - CPU1 fills TLB with old PA
  - Result: two CPUs have different translations for the same VA
    → random memory corruption, crashes, security vulnerabilities

When you might see this in the wild:
  - Poorly ported bootloader that copies PTW config from single-core design
  - Virtualized environments where TCR is misconfigured by hypervisor
  - Early SoC bringup code that doesn't set SH bits

Non-Cacheable + Non-Shareable (NC + NSH):
  IRGN=ORGN=0b00, SH=0b00
  This is "safe" (not UNPREDICTABLE) but very slow:
  All PTW go to DRAM, all CPUs see same DRAM → consistent but 100–800 cycles/miss
  Sometimes used during early boot or for deep debug
```

---

## 5. Outer Shareable vs Inner Shareable

```
When to use Outer Shareable (SH=0b10):
  - If DMA engines or GPU do page table walks and need coherency
  - On some SoCs, the GPU's MMU is Outer Shareable but not Inner
  - If page tables are accessed by agents outside the CPU cluster
  
  On most modern ARM SoCs:
  Inner Shareable = Outer Shareable in practice
  (All agents participate in the same coherency domain)
  → No difference between OSH and ISH for page tables on these SoCs

When they differ:
  - Big.LITTLE SoC with separate A57 cluster (ISH) and A53 cluster (ISH)
    connected via CCI-500 (OSH domain includes both clusters + GPU)
  - Setting SH=ISH means only within-cluster coherency
  - Setting SH=OSH covers both clusters
  - For page tables accessed by CPUs in different clusters: use OSH

Linux always uses ISH (0b11) because on Linux's target hardware,
the ISH domain includes all CPU cores (ARM CCI/CMN makes the whole
SoC ISH-coherent for Normal cacheable memory).
```

---

## 6. SH for Page Tables vs SH for Data

```
Shareability for page table walks (TCR_EL1.SH0/SH1):
  Applied to the PTW hardware when reading PTE entries
  Always set to Inner Shareable by Linux

Shareability for mapped data (PTE descriptor SH[9:8]):
  Applied when a translated access hits the mapped page
  Encoding: 0b00=NSH, 0b10=OSH, 0b11=ISH
  Device memory: 0b00 (Non-Shareable, not applicable to device)
  Normal memory: 0b11 (Inner Shareable) for SMP-coherent data
  
  These are DIFFERENT fields serving different purposes:
  TCR.SH0/SH1 → how to read page tables
  PTE.SH[9:8] → how accesses to the mapped page are treated

Example confusion:
  Q: "Is kernel memory Inner Shareable?"
  A1: Yes, the page table for kernel memory uses Inner Shareable walks (TCR.SH1=0b11)
  A2: Also yes, kernel Normal memory PTEs have SH=0b11 (ISH) for cache coherency
  Both are true but refer to different SH fields!
```

---

## 7. Interview Questions & Answers

**Q1: What shareability setting does Linux use for TCR_EL1.SH, and why?**

Linux sets `SH0 = SH1 = 0b11` (Inner Shareable). This ensures that all CPUs in the coherency domain see a consistent view of page table memory. When one CPU modifies a page table entry (e.g., mapping a new page, setting the Access Flag), the coherent interconnect (CCI/CMN) propagates the update to all other CPUs via cache snooping. Without Inner Shareable, a CPU doing a hardware page table walk might read a stale PTE from its own cache, filling its TLB with wrong physical addresses — resulting in memory corruption.

**Q2: What combination is explicitly UNPREDICTABLE and why?**

`IRGN/ORGN = cacheable` (any WB or WT setting) combined with `SH = Non-Shareable (0b00)` is UNPREDICTABLE per the ARM Architecture Reference Manual. Cacheable + Non-Shareable means page table entries are cached in private L1 caches without any coherency protocol enforcing cross-CPU visibility. One CPU's write remains hidden from other CPUs. Since the hardware PTW reads from these private caches, two CPUs can simultaneously hold different physical address translations for the same virtual address, leading to silent data corruption.

---

## 8. Quick Reference

| SH[1:0] | Meaning | Linux use |
|---|---|---|
| 0b00 | Non-Shareable | Only for NC DRAM walks (debug) |
| 0b01 | Reserved | NEVER use — UNPREDICTABLE |
| 0b10 | Outer Shareable | Some GPU/DMA scenarios |
| 0b11 | Inner Shareable | **Default (always Linux)** |

| Combination | Safe? | Speed | Notes |
|---|---|---|---|
| NC + NSH | Safe | Very slow | Debug only |
| NC + ISH | Safe | Very slow | Coherent, no benefit from ISH since not cached |
| WB + ISH | Safe | Fast | Linux default |
| WB + NSH | UNPREDICTABLE | — | NEVER use on SMP |
| WB + OSH | Safe | Fast | If needed for cross-cluster coherency |
