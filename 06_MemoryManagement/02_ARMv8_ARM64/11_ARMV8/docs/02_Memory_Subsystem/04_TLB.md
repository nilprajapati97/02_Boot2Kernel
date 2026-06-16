# TLB — Translation Lookaside Buffer

## 1. What is a TLB?

The TLB is a **cache for page table translations**. Instead of walking the page
table in memory for every load/store (4+ memory reads), the TLB stores recent
VA→PA translations for instant lookup.

```
  Without TLB: Every memory access = 4-16 reads (page table walk)
  With TLB:    ~99% of accesses = 1-cycle TLB hit

  ┌─────────────┐       ┌──────────────────────────────┐
  │ Virtual Addr │──────▶│            TLB               │
  └─────────────┘       │                              │
                         │  ┌──────────────────────┐   │
                         │  │ VA Tag │ PA │ Attr    │   │
                         │  ├────────┼────┼─────────┤   │
                         │  │ 0x1000 │ PA │ RW,WB   │ HIT → PA out (1 cycle)
                         │  │ 0x2000 │ PA │ RO,Dev  │   │
                         │  │ 0x3000 │ PA │ RW,WB   │   │
                         │  └──────────────────────┘   │
                         │                              │
                         │  MISS → Page Table Walk      │
                         │  (fill TLB with result)      │
                         └──────────────────────────────┘
```

---

## 2. TLB Organization

### Multi-Level TLB Structure

Modern ARM cores use a multi-level TLB:

```
┌─────────────────────────────────────────────────────────────┐
│                    TLB Hierarchy                             │
│                                                               │
│  ┌──────────────────────────────────────────┐               │
│  │  L1 Instruction TLB (L1 ITLB)           │               │
│  │  • 32-48 entries (fully associative)      │               │
│  │  • 1 cycle lookup                         │               │
│  │  • Accessed on instruction fetch          │               │
│  └────────────────────┬─────────────────────┘               │
│                        │ MISS                                │
│  ┌──────────────────────────────────────────┐               │
│  │  L1 Data TLB (L1 DTLB)                  │               │
│  │  • 32-48 entries (fully associative)      │               │
│  │  • 1 cycle lookup                         │               │
│  │  • Accessed on load/store                 │               │
│  └────────────────────┬─────────────────────┘               │
│                        │ MISS                                │
│  ┌────────────────────▼─────────────────────┐               │
│  │  L2 Unified TLB (L2 TLB / Main TLB)     │               │
│  │  • 256-2048+ entries                      │               │
│  │  • 2-8 cycle lookup                       │               │
│  │  • Shared between I and D                 │               │
│  │  • Supports multiple page sizes           │               │
│  └────────────────────┬─────────────────────┘               │
│                        │ MISS                                │
│  ┌────────────────────▼─────────────────────┐               │
│  │  Page Table Walk (Hardware Walker)        │               │
│  │  • 4+ memory accesses                     │               │
│  │  • Walk cache may speed this up           │               │
│  │  • Result fills L2 and L1 TLB             │               │
│  └──────────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────┘
```

### TLB Entry Contents

```
┌──────────────────────────────────────────────────────────────┐
│  TLB Entry                                                    │
│                                                                │
│  ┌───────────┬───────────┬──────┬────┬────┬──────┬─────────┐ │
│  │  VA Tag   │ PA (output)│ ASID │ VM │Size│ Perm │ MemAttr │ │
│  │  (input)  │           │      │ ID │    │      │         │ │
│  └───────────┴───────────┴──────┴────┴────┴──────┴─────────┘ │
│                                                                │
│  VA Tag:    Virtual address bits for matching                  │
│  PA:        Physical address (translation result)              │
│  ASID:      Address Space ID (process identifier)              │
│  VMID:      Virtual Machine ID (for virtualization)            │
│  Size:      Page size (4K, 2M, 1G, etc.)                       │
│  Perm:      Access permissions (R/W/X for EL0/EL1)             │
│  MemAttr:   Memory attributes (type, cacheability, shareability)│
└──────────────────────────────────────────────────────────────┘
```

---

## 3. TLB Lookup Process

```
Step 1: Extract VA fields
  ┌─────────────────────────────────────────┐
  │  Virtual Address                          │
  │  ┌────────────────────┬─────────────────┐│
  │  │  Tag (upper bits)  │  Page Offset     ││
  │  │  (VPN)             │  (not translated)││
  │  └────────────────────┴─────────────────┘│
  └─────────────────────────────────────────┘

Step 2: Lookup in TLB with (VA_tag, ASID, VMID)
  For EACH TLB entry, compare:
    entry.VA_tag == VA_tag?
    entry.ASID == current_ASID (or entry is Global)?
    entry.VMID == current_VMID?
    entry.valid == 1?

Step 3: On HIT
  Physical Address = entry.PA | VA_page_offset
  Check permissions → grant or fault
  Apply memory attributes (cacheability, etc.)

Step 4: On MISS
  Trigger hardware page table walk
  Walk result fills TLB entry
  Retry the access (now TLB hit)
```

---

## 4. TLB Sizes in ARM Cores

```
┌────────────┬──────────┬──────────┬────────────────────┐
│  Core      │  L1 ITLB │  L1 DTLB │  L2 TLB (Unified)  │
├────────────┼──────────┼──────────┼────────────────────┤
│ Cortex-A53 │  10 full │  10 full │  512, 4-way         │
│ Cortex-A55 │  16 full │  16 full │  256-512, 4-way     │
│ Cortex-A72 │  48 full │  32 full │  1024, 4-way        │
│ Cortex-A76 │  48 full │  48 full │  1280, 5-way        │
│ Cortex-A78 │  48 full │  48 full │  1280, 5-way        │
│ Cortex-X2  │  48 full │  48 full │  2048, 8-way        │
│ Neoverse N2│  48 full │  48 full │  2048, 8-way        │
└────────────┴──────────┴──────────┴────────────────────┘

"full" = fully associative (any entry can hold any translation)
```

---

## 5. TLB Maintenance Operations

TLB entries must be invalidated when page tables change:

```
Instruction syntax: TLBI <type><level>{IS|OS}, {Xt}

┌──────────────────────────────────────────────────────────────────┐
│  Operation            Description                                │
├──────────────────────────────────────────────────────────────────┤
│  TLBI ALLE1           Invalidate ALL EL1 entries (local core)   │
│  TLBI ALLE1IS         Invalidate ALL EL1 entries (inner share.) │
│  TLBI VMALLE1IS       Invalidate ALL EL1+EL0 for current VMID  │
│  TLBI VALE1IS, X0     Invalidate by VA, last level, EL1, IS    │
│  TLBI VAE1IS, X0      Invalidate by VA, all levels, EL1, IS    │
│  TLBI ASIDE1IS, X0    Invalidate by ASID, EL1, IS              │
│  TLBI ALLE2           Invalidate ALL EL2 entries                │
│  TLBI IPAS2E1IS, X0   Invalidate by IPA (Stage 2), IS          │
└──────────────────────────────────────────────────────────────────┘

IS = Inner Shareable (broadcast to all cores in inner shareable domain)
OS = Outer Shareable (broadcast to all cores in outer shareable domain)

IMPORTANT: TLBI followed by DSB + ISB to ensure completion:
  TLBI VALE1IS, X0      // Invalidate
  DSB ISH               // Wait for invalidation to complete on all cores
  ISB                    // Flush pipeline
```

### TLBI Operand Format

```
X0 format for VA-based TLBI:
┌───────────────────┬──────────────┬──────────────┐
│  Bits [63:48]     │  Bits [47:44]│  Bits [43:0] │
│  ASID             │  TTL (level) │  VA[55:12]   │
│  (16 bits)        │  (opt, v8.4) │ (page number)│
└───────────────────┴──────────────┴──────────────┘

TTL (ARMv8.4): Specifies which page table level to invalidate
  → More efficient than invalidating all levels
  00 = any level, 01 = L1, 10 = L2, 11 = L3
```

---

## 6. TLB Coherency & Shootdown

When one core modifies a page table, other cores may have stale TLB entries:

```
┌──────────────────────────────────────────────────────────┐
│  Core 0                    Core 1                         │
│  ──────                    ──────                         │
│  Modifies page table       Has old TLB entry             │
│  (unmap VA 0x1000)         (VA 0x1000 → PA 0x5000)      │
│                                                           │
│  TLBI VAE1IS, X0  ────────▶  TLB entry invalidated      │
│  DSB ISH          ────────▶  Invalidation complete       │
│  ISB                                                      │
│                                                           │
│  Now safe: Core 1 will    Core 1 sees TLB miss,         │
│  not use stale mapping    re-walks → gets fault          │
└──────────────────────────────────────────────────────────┘

This process is called "TLB shootdown" (x86 term) or
"TLB broadcast invalidation" (ARM term).

The IS/OS suffix makes these TLBI operations broadcast.
Without IS: Only local core's TLB is invalidated!
```

---

## 7. CnP — Common Not Private (ARMv8.2)

CnP allows cores in the same cluster to share TLB entries:

```
Without CnP:
  Core 0 walks page table → fills its TLB
  Core 1 walks same page table → fills its TLB (redundant work!)

With CnP (TTBR_EL1.CnP = 1):
  Core 0 walks page table → fills shared TLB
  Core 1 accesses same VA → TLB HIT (saved a walk!)

Requirements:
  • All cores must have identical TTBR / TCR / MAIR configuration
  • ASID must be consistent across cores sharing the TLB
  • Improves TLB hit rate for shared address spaces (kernel)
```

---

## 8. Performance Impact

```
TLB miss penalty analysis:

  4 KB page, 48-bit VA, 4-level walk:
    Best:   ~16 cycles (all page table entries in L1 cache)
    Typical: ~30-50 cycles (L2/L3 cached)
    Worst:  ~400 cycles (cold, goes to DRAM)

  With 2 MB huge pages:
    Only 3 levels to walk (L0→L2)
    And one TLB entry covers 512× more memory
    → TLB miss rate drops dramatically

  TLB miss rates:
    Typical: <1% for most workloads with sufficient TLB entries
    Database/HPC: Can be 5-10% with small pages → use huge pages

  Optimizations:
    1. Use huge pages (2 MB / 1 GB)
    2. Use contiguous hint in PTEs (16× 4 KB = 64 KB in one TLB entry)
    3. Walk caches (cache intermediate levels)
    4. CnP (share TLB entries across cores)
    5. Keep hot data within few pages to maximize TLB hit rate
```

---

Next: [Memory Ordering & Barriers →](./05_Memory_Ordering.md)
