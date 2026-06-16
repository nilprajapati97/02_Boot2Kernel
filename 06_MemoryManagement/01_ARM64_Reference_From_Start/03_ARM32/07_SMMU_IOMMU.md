# ARM SMMU and IOMMU Architecture
## Document 7: System MMU, IOVA, DMA Mapping, Qualcomm and NVIDIA

**Author:** Senior Kernel Engineer  
**Target:** ARM SMMU v1/v2, Linux IOMMU Framework, ARMv7-A SoCs  
**Scope:** SMMU hardware, Linux IOMMU framework, Qualcomm/NVIDIA specifics, debugging  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview), Document 05 (Cache & DMA Coherency)

---

## Table of Contents
1. [Why IOMMU / SMMU?](#1-why-iommu--smmu)
2. [SMMU Hardware Architecture](#2-smmu-hardware-architecture)
3. [SMMU v1/v2 Translation — Deep Dive](#3-smmu-v1v2-translation--deep-dive)
4. [SMMU Page Table Formats](#4-smmu-page-table-formats)
5. [Linux IOMMU Framework](#5-linux-iommu-framework)
6. [IOVA Space Management](#6-iova-space-management)
7. [DMA Mapping Through SMMU](#7-dma-mapping-through-smmu)
8. [Qualcomm SMMU (qcom-iommu / arm-smmu)](#8-qualcomm-smmu-qcom-iommu--arm-smmu)
9. [NVIDIA GPU IOMMU and CUDA UVM](#9-nvidia-gpu-iommu-and-cuda-uvm)
10. [AMD IOMMU (IOMMUv2)](#10-amd-iommu-iommuv2)
11. [SMMU for Security and Isolation](#11-smmu-for-security-and-isolation)
12. [Performance Considerations](#12-performance-considerations)
13. [Debugging SMMU Issues](#13-debugging-smmu-issues)

---

## 1. Why IOMMU / SMMU?

### 1.1 Problem: DMA Without IOMMU

```
Without IOMMU/SMMU:

┌─────────┐  physical address  ┌──────────────┐
│   CPU   │ ─────────────────→ │    DRAM      │
│  (MMU)  │                    │              │
└─────────┘                    │  0x00001000: │
                                │  secret_key  │
┌─────────┐  physical address  │              │
│   DMA   │ ─────────────────→ └──────────────┘
│ engine  │
│ (no MMU)│
└─────────┘
  ↑
  A compromised/buggy device driver can instruct DMA to read/write
  ANY physical address — bypasses CPU MMU completely!

Real examples:
  - Thunderbolt DMA attacks (Thunderclap 2019): 
    Plug in malicious device → read host RAM at will
  - WiFi/USB firmware bugs → DMA to kernel text → arbitrary code execution
  - GPU driver bugs → DMA scatter-gather to wrong PA
```

### 1.2 Solution: SMMU (System MMU)

```
With SMMU:

┌─────────┐  virtual address   ┌──────────┐  physical  ┌──────────────┐
│   CPU   │ ─────────────────→ │ CPU MMU  │ ─────────→ │    DRAM      │
│         │                    └──────────┘            │              │
└─────────┘                                            │  0x00001000: │
                                                       │  secret_key  │
┌─────────┐  IOVA (IO virt)    ┌──────────┐  physical │              │
│   DMA   │ ─────────────────→ │   SMMU   │ ─────────→ │  only mapped │
│ engine  │                    │          │            │  DMA buffer  │
└─────────┘                    └──────────┘            └──────────────┘

SMMU capabilities:
  ✓ Restrict DMA to only its allocated IOVA range
  ✓ Translate IOVA → PA (device sees IOVA, not real PA)
  ✓ Enforce per-device isolation (each device gets separate IOVA space)
  ✓ Support Stage-2 (hypervisor) isolation of device DMA
  ✓ Enable physical memory scatter-gather for DMA (device thinks memory is contiguous)
```

### 1.3 SMMU Use Cases

| Use Case | Description |
|----------|-------------|
| Security isolation | Each device can only DMA to its own IOVA space |
| Non-contiguous DMA | Device sees contiguous IOVA, physical pages can be scattered |
| Virtualization | VMs get isolated IOVA → guest PA, SMMU does Stage-2 → host PA |
| 32-bit device in 64-bit system | Map 64-bit PA into 32-bit IOVA space for legacy device |
| Qualcomm peripheral isolation | GPU, modem, DSP each have separate SMMU context |
| NVIDIA GPU | PCIe IOMMU for GPU DMA to system memory |

---

## 2. SMMU Hardware Architecture

### 2.1 SMMU Block Diagram

```
┌──────────────────────────────────────────────────────────────────────┐
│                      ARM SMMU v2 (SMMUv2)                           │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │             Stream Matching / Indexing Unit                   │    │
│  │                                                               │    │
│  │  Stream ID (SID) → Context Bank selection                    │    │
│  │  SMR (Stream Match Register): mask-based matching            │    │
│  │  S2CR (Stream-to-Context): maps SID → context bank index     │    │
│  └────────────────────────────┬────────────────────────────────┘    │
│                                │                                      │
│  ┌─────────────────────────────▼────────────────────────────────┐   │
│  │                  Context Banks (N banks)                      │   │
│  │                                                               │   │
│  │  Each context bank = independent address space:               │   │
│  │  ┌─────────────────────────────────────────────────────┐    │   │
│  │  │  CB0: GPU DMA                                         │   │    │
│  │  │    CBA2R: Stage 1 (IOVA→IPA) format                 │   │    │
│  │  │    CBAR:  Stage 1, Non-Secure                        │   │    │
│  │  │    TTBRn: Page table base for GPU IOVA space         │   │    │
│  │  │    SCTLR: Enable translation, caching                │   │    │
│  │  │    FAR/FSR: Fault address/status                     │   │    │
│  │  └─────────────────────────────────────────────────────┘   │    │
│  │  ┌─────────────────────────────────────────────────────┐    │   │
│  │  │  CB1: USB DMA                                        │   │    │
│  │  └─────────────────────────────────────────────────────┘    │   │
│  │  ┌─────────────────────────────────────────────────────┐    │   │
│  │  │  CB2: Modem DMA (Secure context)                     │   │    │
│  │  └─────────────────────────────────────────────────────┘    │   │
│  └────────────────────────────┬────────────────────────────────┘   │
│                                │                                      │
│  ┌─────────────────────────────▼────────────────────────────────┐   │
│  │               Translation Cache (TLB)                         │   │
│  │   Per-context-bank TLB entries (ASID-tagged)                 │   │
│  │   SMMU TLBIALL / TLBIVMID / TLBIASID / TLBIVA               │   │
│  └────────────────────────────┬────────────────────────────────┘   │
│                                │                                      │
│              AXI port: filtered (translated) PA out                  │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 Stream ID and Context Bank Mapping

```
Stream ID (SID): Identifies the DMA master (which device is doing DMA)
  - Assigned by SoC fabric: each master has a unique SID
  - Qualcomm MSM8996: GPU SID=10, USB SID=4, Camera SID=7
  - NVIDIA Tegra: Each engine has assigned SID

SMR (Stream Match Register) + S2CR (Stream to Context Register):
  Up to 128 SMR registers (SMMUv2)
  
  SMR[n] format:
    [31]    Valid
    [30:16] Mask    (0=must match, 1=don't care)
    [14:0]  ID      (base SID value)
  
  S2CR[n] format:
    [17:16] Type    (0=translation, 1=bypass, 2=fault)
    [7:0]   CBNDX   (context bank index)
  
  Lookup: For each DMA transaction with SID:
    For each SMR: if (SID & ~mask) == ID → match
    Use corresponding S2CR → CBNDX → use context bank CBNDX
    
Example setup (GPU SID=10 → CB0):
  SMR[0] = {valid=1, mask=0x0000, id=0x000A}  @ match SID==10 exactly
  S2CR[0] = {type=0, CBNDX=0}                  @ translate using CB0
```

---

## 3. SMMU v1/v2 Translation — Deep Dive

### 3.1 Translation Stages

```
SMMU supports 1 or 2 translation stages:

Stage 1 only (S1):
  IOVA ──[S1 PT]──→ PA
  Used: Linux DMA API (device driver allocates IOVA, maps to physical pages)
  Context: CBA2R.VA64=0 → AArch32 short-descriptor page tables
           CBA2R.VA64=1 → AArch64 4-level page tables (even on 32-bit SMMU)

Stage 2 only (S2):
  IPA ──[S2 PT]──→ PA
  Used: Hypervisor (KVM for ARM): guest physical → host physical
  Context: CBAR type = Stage-2

Stage 1 + Stage 2 (nested):
  IOVA ──[S1 PT]──→ IPA ──[S2 PT]──→ PA
  Used: VM with IOMMU passthrough: VM controls S1, hypervisor controls S2
  Context: CBAR type = Stage-1 with Stage-2 walk

Bypass:
  IOVA passes through as PA (no translation)
  Used: During IOMMU init, or for trusted devices
  S2CR type = bypass
```

### 3.2 Stage-1 Translation (IOVA → PA)

```
SMMUv2 Stage-1 uses same format as CPU ARM32 MMU:

AArch32 Short Descriptor (most common on ARM32 SMMU):
  Uses same L1/L2 descriptor format as CPU MMU (see Document 01)
  CB.TTBR0 = L1 page table base (IOVA[31:20] selects L1 entry)
  CB.TTBR1 = optional second page table (for larger IOVA spaces)
  CB.TCR (TTBCR equivalent): N field, IRGN, ORGN, SH

AArch64 Long Descriptor (preferred for 64-bit IOVAs even on ARM32 SMMU):
  4-level: PGD → PUD → PMD → PTE
  Allows > 4GB IOVA space
  CB.CBA2R.VA64 = 1 to use this format

ARM SMMU v3 (ARM64 era):
  Uses STE (Stream Table Entry) instead of SMR/S2CR
  CD (Context Descriptor) instead of CB registers
  Supports PCIe ATS (Address Translation Services)
```

### 3.3 Context Bank Registers

```c
/* Key context bank registers (base: SMMU_CB_BASE + CBn * CB_SIZE) */

#define ARM_SMMU_CB_SCTLR       0x000   /* Stage 1 Control: enable, caching */
#define ARM_SMMU_CB_ACT         0x004   /* Aux control */
#define ARM_SMMU_CB_RESUME      0x008   /* Stall/fault resume */
#define ARM_SMMU_CB_TCR2        0x010   /* Translation control (ext) */
#define ARM_SMMU_CB_TTBR0       0x020   /* L1 table base (IOVA low half) */
#define ARM_SMMU_CB_TTBR1       0x028   /* L1 table base (IOVA high half) */
#define ARM_SMMU_CB_TCR         0x030   /* Translation control (TTBCR equiv) */
#define ARM_SMMU_CB_CONTEXTIDR  0x034   /* ASID for this context */
#define ARM_SMMU_CB_MAIRn       0x038   /* Memory attribute indices */
#define ARM_SMMU_CB_FSR         0x058   /* Fault status register */
#define ARM_SMMU_CB_FAR         0x060   /* Fault address register */
#define ARM_SMMU_CB_FSYNR0      0x068   /* Fault syndrome 0 */

/* SCTLR bits */
#define CB_SCTLR_M      (1 << 0)   /* Enable translation */
#define CB_SCTLR_TRE    (1 << 1)   /* TEX remap enable */
#define CB_SCTLR_AFE    (1 << 2)   /* Access flag enable */
#define CB_SCTLR_CFIE   (1 << 6)   /* Context fault interrupt enable */
#define CB_SCTLR_CFRE   (1 << 7)   /* Context fault report enable */
#define CB_SCTLR_HUPCF  (1 << 8)   /* Halt on unrecov PCIe fault */
```

---

## 4. SMMU Page Table Formats

### 4.1 ARM32 Short Descriptor in SMMU

```
Same as CPU ARM32 page tables — most code is shared:
  L1 table: 4096 × 4-byte entries, 16KB, 16KB aligned
  L1[i] covers 1MB of IOVA
  L2 table: 256 × 4-byte entries, 1KB (used in 2KB pair like CPU ARM32)

Key difference from CPU page tables:
  - No hardware access flag management (software must set AF=1 initially)
  - No hardware dirty bit (software dirty tracking via write-protect + fault)
  - SMMU does NOT use ASID for isolation — uses context banks instead
  - SMMU ASID (in CB.CONTEXTIDR) is for SMMU internal TLB only

Memory attributes:
  TEX/C/B same encoding as CPU
  For DMA coherent: Normal NC (TEX=001, C=0, B=0)
  For MMIO passthrough: Strongly Ordered (TEX=000, C=0, B=0)
```

### 4.2 AArch64 Long Descriptor in SMMU (preferred)

```
4-level page table structure:
  Level 0: 512 PGD entries × 512GB each (512×512GB = 256TB total)
  Level 1: 512 PUD entries × 1GB each
  Level 2: 512 PMD entries × 2MB each
  Level 3: 512 PTE entries × 4KB each

SMMU CB registers for AArch64:
  CB.CBA2R.VA64 = 1       ← enable AArch64 page tables
  CB.TCR.TG0 = 0          ← 4KB granule
  CB.TCR.T0SZ = 32        ← IOVA size = 2^(64-32) = 4GB (for 32-bit IOVA space)
  CB.TCR.SL0 = 1          ← Start at level 1 (for T0SZ=32, 4GB)
  CB.TTBR0 = L1_table_pa  ← Physical address of level-1 table
```

---

## 5. Linux IOMMU Framework

### 5.1 Software Stack

```
┌────────────────────────────────────────────────────────────────┐
│                      Device Driver                             │
│  dma_alloc_coherent() / dma_map_single()                       │
│  Operates in IOVA space                                        │
└────────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌────────────────────────────────────────────────────────────────┐
│                  DMA Mapping Layer                             │
│  kernel/dma/mapping.c                                          │
│  Calls iommu_dma_ops if IOMMU present,                        │
│  else direct DMA ops (no translation)                          │
└────────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌────────────────────────────────────────────────────────────────┐
│                  IOMMU Core (drivers/iommu/)                   │
│                                                                 │
│  iommu_domain: Abstract domain (IOVA space)                   │
│  iommu_group: Set of devices sharing an IOVA domain           │
│  iommu_ops: Per-SMMU callbacks                                │
│                                                                 │
│  iommu_map(domain, iova, paddr, size, prot)                   │
│  iommu_unmap(domain, iova, size)                               │
│  iommu_iova_to_phys(domain, iova)                             │
└────────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌────────────────────────────────────────────────────────────────┐
│           SMMU Driver (drivers/iommu/arm-smmu.c)              │
│           or Qualcomm (drivers/iommu/qcom_iommu.c)            │
│                                                                 │
│  arm_smmu_map() → updates SMMU page tables                    │
│  arm_smmu_tlbi_cb_asid() → TLB invalidate in SMMU            │
└────────────────────────────────────────────────────────────────┘
                           │
                           ▼
                    ARM SMMU Hardware
```

### 5.2 iommu_domain

```c
/* include/linux/iommu.h */
struct iommu_domain {
    unsigned type;                  /* IOMMU_DOMAIN_DMA, IOMMU_DOMAIN_UNMANAGED */
    const struct iommu_ops *ops;    /* SMMU driver callbacks */
    unsigned long pgsize_bitmap;    /* Supported page sizes (4KB, 64KB, 1MB) */
    iommu_fault_handler_t handler;  /* Called on SMMU fault */
    void *handler_token;
    struct iommu_domain_geometry geometry; /* IOVA range */
    struct iommu_dma_cookie *iova_cookie;  /* IOVA allocator */
};

/* Domain types */
#define IOMMU_DOMAIN_BLOCKED    0    /* All DMA blocked (strict mode) */
#define IOMMU_DOMAIN_IDENTITY   1    /* IOVA == PA (passthrough) */
#define IOMMU_DOMAIN_UNMANAGED  2    /* Driver manages IOVA manually */
#define IOMMU_DOMAIN_DMA        3    /* Kernel manages IOVA (dma-iommu) */
```

### 5.3 iommu_group — Device Grouping

```
iommu_group: A set of devices that MUST share an IOVA domain
             because they cannot be isolated from each other at HW level

Typical grouping:
  - PCI devices behind a PCIe switch that shares transactions → one group
  - Integrated DMA engines that share stream IDs → one group
  - Each standalone device → its own group (ideal)

iommu_group_add_device(group, dev)
iommu_attach_group(domain, group)  ← attach domain to group
```

### 5.4 IOMMU Map/Unmap

```c
/* Map a physical address range into an IOMMU domain */
int iommu_map(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot)
{
    /* 1. Determine page size (use largest supported: 1MB/64KB/4KB) */
    /* 2. For each page: update SMMU page table (CB.TTBR0-based tables) */
    /* 3. Flush SMMU TLB for the IOVA range */
    /* 4. Return 0 on success */
}

/* prot flags */
#define IOMMU_READ    (1 << 0)   /* DMA read access */
#define IOMMU_WRITE   (1 << 1)   /* DMA write access */
#define IOMMU_CACHE   (1 << 2)   /* Page tables are cacheable */
#define IOMMU_NOEXEC  (1 << 3)   /* XN bit in SMMU descriptor */
#define IOMMU_MMIO    (1 << 4)   /* Device memory type (for passthrough) */
```

---

## 6. IOVA Space Management

### 6.1 IOVA Allocator

```c
/* drivers/iommu/iova.c */
/*
 * IOVA allocator: manages a virtual address space [start, limit]
 * Provides: alloc_iova(), free_iova()
 * Uses: Red-black tree of free/used ranges + per-CPU caches
 */

struct iova_domain {
    spinlock_t iova_rbtree_lock;
    struct rb_root rbroot;           /* RB tree of iova ranges */
    struct rb_node *cached_node;     /* MRU cache */
    struct rb_node *cached32_node;   /* 32-bit MRU cache */
    unsigned long granule;           /* Minimum alignment (page size) */
    unsigned long start_pfn;         /* IOVA start >> PAGE_SHIFT */
    unsigned long dma_32bit_pfn;     /* Limit for 32-bit capable devices */
    struct iova_rcache rcaches[IOVA_RANGE_CACHE_MAX_SIZE]; /* per-CPU cache */
};

/* Allocation: reverse search (from top) for optimal 32-bit IOVA */
struct iova *alloc_iova(struct iova_domain *iovad, unsigned long size,
                         unsigned long limit_pfn, bool size_aligned)
{
    /* 1. Check per-CPU free cache first (lock-free fast path) */
    /* 2. Slow path: walk rb tree to find free range */
    /* 3. Insert new iova into rb tree */
    /* 4. Return IOVA start PFN */
}
```

### 6.2 IOVA Caching

```
IOVA allocator performance optimization:

Per-CPU rcache (recent free cache):
  Size buckets: 1 page, 2 pages, 4 pages, 8 pages, ... (powers of 2)
  Each CPU maintains a stack of recently freed IOVAs of each size
  
  Free path: push to rcache (no lock needed if CPU-local)
  Alloc path: pop from rcache (no lock, ~10 ns)
  Fallback: rb-tree search (lock needed, ~100 ns)

Result: dma_map_single() for small buffers is nearly lock-free
        Benefit: High-frequency DMA (USB, audio, network) doesn't serialize
```

---

## 7. DMA Mapping Through SMMU

### 7.1 dma_map_single() with SMMU

```c
/*
 * With SMMU: dma_map_single calls into iommu_dma_map_page()
 */
dma_addr_t dma_map_single(struct device *dev, void *ptr, size_t size,
                            enum dma_data_direction dir)
{
    /* 1. Get physical address */
    phys_addr_t pa = virt_to_phys(ptr);

    /* 2. Allocate IOVA (may be different from PA!) */
    struct iova *iova = alloc_iova(&domain->iova_domain,
                                    PAGE_ALIGN(size) >> PAGE_SHIFT,
                                    dma_get_mask(dev) >> PAGE_SHIFT, true);
    dma_addr_t iova_start = iova_dma_addr(iovad, iova);

    /* 3. Map IOVA → PA in SMMU page tables */
    iommu_map(domain, iova_start, pa, size, IOMMU_READ | IOMMU_WRITE);

    /* 4. Flush SMMU TLB (invalidate old/stale entry) */
    domain->ops->iotlb_range_add(domain, iova_start, size);

    /* 5. Perform CPU cache maintenance (same as without SMMU) */
    arch_sync_dma_for_device(pa, size, dir);

    return iova_start;  /* Return IOVA — device programs this into its DMA register */
}
```

### 7.2 Why IOVA ≠ PA Matters

```
Scatter-Gather Unification:
  Physical pages: PA0=0x80001000, PA1=0x80003000, PA2=0x90005000 (scattered)
  SMMU IOVA:      0x10000000–0x10002FFF (contiguous IOVA)
  
  SMMU maps:
    IOVA[0x10000000] → PA0=0x80001000
    IOVA[0x10001000] → PA1=0x80003000
    IOVA[0x10002000] → PA2=0x90005000
  
  Device programs one DMA descriptor: src=0x10000000, len=3*4KB
  Device reads contiguous IOVA → SMMU translates each 4KB separately
  → Device gets data from scattered physical pages seamlessly

32-bit Device in 64-bit System:
  Device DMA mask: 32-bit (max PA=0xFFFFFFFF)
  Physical RAM may be above 4GB (0x100000000)
  SMMU IOVA space: 0x00000000–0xFFFFFFFF (32-bit IOVA)
  SMMU maps 32-bit IOVA → 64-bit PA
  Device happily DMA's to "32-bit PA" which SMMU remaps to high physical RAM
```

---

## 8. Qualcomm SMMU (qcom-iommu / arm-smmu)

### 8.1 Qualcomm SMMU Implementation History

```
Qualcomm SMMU generations:

MSM7xxx/MSM8xxx (older): Custom qcom-iommu driver
  File: drivers/iommu/qcom-iommu.c (older kernels)
  Different register layout from ARM SMMU spec
  Context banks: limited, simpler design

MSM8996+ (Snapdragon 820+): arm-smmu driver (SMMU v2 compliant)
  File: drivers/iommu/arm-smmu.c + arm-smmu-impl.c
  Standard ARM SMMU v2 with Qualcomm quirks
  
Snapdragon 845+: ARM SMMU v2 with Qualcomm-specific features:
  - Secure and Non-Secure context banks
  - IOMMU pagetable formats: AARCH64 (preferred) and AARCH32
  - Per-CB performance monitors
  - Qualcomm's custom IOMMU domain flags: IOMMU_USE_UPSTREAM_HINT

SD 8 Gen 1+: Transitioning to arm-smmu-v3 (SMMU v3)
```

### 8.2 Qualcomm Stream ID Assignment

```
Qualcomm SoC (Snapdragon 845 example):

Master      Stream ID   Context Bank    Domain
────────    ─────────   ────────────    ──────
GPU         0x009       CB0             GPU IOVA domain
MDP (disp)  0x010       CB1             Display DMA domain
Camera      0x011,012   CB2             Camera DMA domain
Video enc   0x013       CB3             Video encoder domain
USB3 DMA    0x020       CB4             USB DMA domain
SD/eMMC     0x021       CB5             Storage DMA domain
Modem IPA   0x050–0x5F  CB6 (Secure)   Modem isolated domain
Crypto eng  0x030       CB7 (Secure)   Secure crypto domain

Modem domain: CB6 is programmed by QSEE (Secure world)
  NS Linux cannot access CB6 registers → modem DMA fully isolated
  
Android use:
  ION (now DMA-BUF) heaps assigned per-device IOMMU domain
  Camera driver: maps camera buffers into Camera IOVA domain only
  Camera firmware cannot DMA to GPU buffers (different IOVA domain)
```

### 8.3 Qualcomm IOMMU Fault Handling

```c
/* Qualcomm arm-smmu fault handler */
static irqreturn_t arm_smmu_context_fault(int irq, void *dev)
{
    u32 fsr, fsynr, cbfrsynra;
    unsigned long iova;
    struct arm_smmu_domain *smmu_domain = dev;
    struct arm_smmu_device *smmu = smmu_domain->smmu;
    int idx = smmu_domain->cfg.cbndx;

    fsr  = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSR);
    iova = arm_smmu_cb_readq(smmu, idx, ARM_SMMU_CB_FAR);
    fsynr = arm_smmu_cb_read(smmu, idx, ARM_SMMU_CB_FSYNR0);
    cbfrsynra = arm_smmu_gr1_read(smmu, ARM_SMMU_GR1_CBFRSYNRA(idx));

    dev_err(smmu->dev,
            "SMMU fault on context bank %d [%s]\n"
            "  FSR = 0x%08x [%s%s%s%s%s%s%s]\n"
            "  FAR = 0x%016lx\n"
            "  FSYNR0 = 0x%08x, CBFRSYNRA = 0x%08x\n",
            idx, smmu_domain->name,
            fsr,
            (fsr & FSR_TF)   ? "Translation " : "",
            (fsr & FSR_AFF)  ? "AccessFlag " : "",
            (fsr & FSR_PF)   ? "Permission " : "",
            (fsr & FSR_EF)   ? "External " : "",
            (fsr & FSR_TLBLKF) ? "TLBLock " : "",
            (fsr & FSR_TLBMCF) ? "TLBMultiHit " : "",
            (fsr & FSR_SS)   ? "Stall " : "",
            iova, fsynr, cbfrsynra);

    /* Clear fault */
    arm_smmu_cb_write(smmu, idx, ARM_SMMU_CB_FSR, fsr);
    return IRQ_HANDLED;
}
```

---

## 9. NVIDIA GPU IOMMU and CUDA UVM

### 9.1 NVIDIA PCIe IOMMU for GPU

```
NVIDIA GPU DMA model:

Traditional (pre-UVM):
  GPU VRAM: separate, not accessible from CPU
  Host memory DMA: GPU DMA controller uses PCIe DMA
  IOMMU on PCIe side: CPU IOMMU (x86 Intel VT-d or AMD IOMMU)
  
  Flow:
    CUDA malloc(host, size)
    GPU programs DMA: src=host_pa, dst=VRAM
    DMA traverses PCIe → CPU IOMMU → host DRAM
    IOMMU checks: is this SID allowed to access this PA? → allows if mapped

  ARM SoC NVIDIA Tegra:
    GPU uses SMMU instead of PCIe IOMMU
    Tegra SMMU: ARM SMMU v1 compatible, custom Tegra register layout
    GPU gets its own IOVA domain (context bank in Tegra SMMU)
```

### 9.2 CUDA Unified Virtual Memory (UVM)

```
CUDA UVM: CPU and GPU share a single virtual address space

Traditional CUDA (memcpy explicit):
  CPU allocates host_buf (PA=0x80000000)
  GPU allocates device_buf (VRAM PA=0xA0000000)
  cudaMemcpy(device_buf, host_buf, size, H2D)  ← explicit DMA
  GPU kernel reads device_buf
  cudaMemcpy(host_buf, device_buf, size, D2H)  ← explicit DMA

CUDA UVM (transparent):
  void *uvm_ptr;
  cudaMallocManaged(&uvm_ptr, size);  ← "virtual" allocation
  // uvm_ptr has same value in CPU and GPU address spaces
  
  CPU: uvm_ptr[i] = value;  → page fault → CUDA UVM driver
  GPU: result = uvm_ptr[j]; → GPU page fault → CUDA UVM driver
       (GPU has its own MMU: Pascal+ has hardware page fault support)
  
  UVM migration:
    When CPU accesses GPU page → UVM moves page to CPU-accessible memory
    When GPU accesses CPU page → UVM moves page to VRAM (or DMA maps it)
    IOMMU used to map pages for DMA when not migrated

ARM implementation:
  Tegra Jetson: GPU (Pascal/Volta/Ampere) + ARM CPU + shared SMMU
  GPU MMU + SMMU both map same physical pages
  UVM relies on SMMU for CPU↔GPU coherent access
```

### 9.3 NVIDIA IOMMU in Linux

```c
/* drivers/iommu/tegra-smmu.c (Tegra SMMU implementation) */

/*
 * Tegra SMMU differences from ARM SMMU:
 *  - Different register offsets
 *  - Uses 32-bit page tables only (Tegra K1/X1 era)
 *  - TLB flush: write SMMU_TLB_FLUSH = 0x1 (not MCR instruction)
 */

static int tegra_smmu_map(struct iommu_domain *domain, unsigned long iova,
                           phys_addr_t pa, size_t size, int prot)
{
    struct tegra_smmu_as *as = to_smmu_as(domain);
    struct page *page;
    u32 *pd;

    /* Get or allocate L2 page table for this IOVA */
    pd = tegra_smmu_pdir_page(as, iova);

    /* Set L2 entry: PA >> PAGE_SHIFT, attributes */
    pd[SMMU_L2_IDX(iova)] = SMMU_PTE_PA(pa) | SMMU_PTE_ATTR(prot);

    /* Flush SMMU TLB */
    tegra_smmu_flush_tlb_as(as, iova, size);

    return 0;
}
```

---

## 10. AMD IOMMU (IOMMUv2)

### 10.1 AMD IOMMU Architecture

```
AMD IOMMU (AMD-Vi / IOMMU v2):
  Defined in AMD IOMMU Architecture spec (AMD reference manual 48882)
  
  Device Table:
    4096-byte entry per PCI requestor ID (Bus/Dev/Func)
    DTE (Device Table Entry): 32 bytes per device
    Holds: domain ID, page table pointer, enable flags
  
  Event Log:
    Ring buffer for IOMMU faults (similar to SMMU fault register)
    
  PPR (Peripheral Page Request) Log:
    Devices can request CPU to resolve a fault (PCIe ATS extension)
    
  IOMMUv2 Specific (vs v1):
    Guest Translation: devices can have two-level translation
    PRI (Page Request Interface): device sends page fault to CPU
    PASID (Process Address Space ID): per-PASID translation tables

Difference from ARM SMMU:
  ARM SMMU: Context banks (CB) → page tables
  AMD IOMMU: Device Table entries (DTE) → page tables
  AMD IOMMU: domain_id used instead of ASID
```

### 10.2 AMD IOMMU in Linux

```c
/* drivers/iommu/amd/iommu.c */

static int amd_iommu_map(struct iommu_domain *dom, unsigned long iova,
                          phys_addr_t paddr, size_t page_size, int iommu_prot)
{
    struct protection_domain *domain = to_pdomain(dom);
    int ret;

    /* AMD uses 4-level page tables (same format as x86-64 CPU) */
    /* This allows sharing page tables between CPU and IOMMU */
    ret = iommu_map_page(domain, iova, paddr, page_size,
                          (iommu_prot & IOMMU_READ)  ? IOMMU_PROT_IR : 0 |
                          (iommu_prot & IOMMU_WRITE) ? IOMMU_PROT_IW : 0,
                          GFP_ATOMIC);

    if (!ret)
        domain_flush_tlb(domain);  /* Flush IOMMU TLB */

    return ret;
}
```

---

## 11. SMMU for Security and Isolation

### 11.1 Android ION and SMMU Isolation

```
Android multimedia stack uses ION/DMA-BUF for zero-copy sharing.
Without SMMU: all processes can read ION buffers (PA known)
With SMMU: each process/driver gets isolated IOVA domain

Android use-case:
  Camera captures → CameraBuffer (ION heap, SMMU domain: camera)
  Display reads   → maps CameraBuffer into display SMMU domain
  GPU processes   → maps CameraBuffer into GPU SMMU domain

  CameraBuffer can only be accessed via its mapped IOVA in each domain.
  A rogue process that gets CameraBuffer PA cannot DMA to it
  (SMMU blocks: unauthorized stream ID → camera domain PA)

Content protection (DRM):
  Protected video buffer: PA only accessible from Secure SMMU context
  Decoder DMA (in Secure context) → writes protected PA
  Display hardware (mapped in Secure SMMU context) → reads protected PA
  GPU or CPU → SMMU fault (not in their IOVA mapping)
```

### 11.2 Strict Mode vs Passthrough Mode

```c
/*
 * Strict IOMMU mode (default in security-hardened kernels):
 *   All devices use IOMMU translation
 *   Devices not attached to a domain: SMMU_GBPA = FAULT
 *   Any unmapped DMA access → SMMU fault → device gets error
 *
 * Passthrough mode (performance, trusted device):
 *   S2CR type = BYPASS → IOVA == PA
 *   No translation overhead
 *   Security: trust device not to read arbitrary PA
 *
 * Linux IOMMU strict DMA mode:
 */
kernel command line: iommu=strict      ← default fault on unmap
                     iommu=pt          ← passthrough (perf, no security)
                     arm-smmu.disable_bypass=1  ← force all to domain
```

---

## 12. Performance Considerations

### 12.1 SMMU TLB Miss Overhead

```
SMMU TLB hit:  ~2-5 cycles (same access latency as without SMMU)
SMMU TLB miss: ~20-40 cycles (page table walk by SMMU PTW)
SMMU page table walk (RAM miss): ~100-200 cycles

Optimization:
  1. SMMU page tables in cached memory (ARM SMMU v2 supports caching):
     CB.TCR.IRGN = 0b01 (Inner WB, RA, WA)
     CB.TCR.ORGN = 0b01 (Outer WB, RA, WA)
  
  2. Use large IOMMU pages (1MB) for kernel DMA:
     Fewer TLB entries needed for large buffers
     
  3. SMMU TLB batching:
     Accumulate IOMMU unmap operations, flush TLB once
     arm_smmu_iotlb_sync() called in batch from iommu_unmap_fast()
  
  4. IOPTE caching (ARM SMMU v2):
     SMMU caches page table entries in hardware
     Stall-on-fault mode: less penalty for faults vs terminate mode
```

### 12.2 IOVA Allocator Bottleneck

```
High-frequency DMA workloads (100K DMA/sec):
  Each dma_map_single: alloc IOVA + map SMMU PT + cache flush
  Each dma_unmap_single: unmap SMMU PT + TLB invalidate + free IOVA
  
  bottleneck: IOVA tree lock (rb-tree insertion/deletion)

Solutions:
  1. Per-CPU IOVA rcache: avoid lock for common sizes
  2. IOVA_CACHE_MAX_SIZE: pre-allocate large IOVA chunks
  3. swiotlb (software bounce buffer): bypass SMMU for small, fast transfers
  4. dma_map_sg(): batch multiple pages into one SMMU operation

Measurement:
  Without IOMMU: 15M DMA ops/sec (direct PA)
  With IOMMU + rcache: 10M DMA ops/sec
  With IOMMU + no rcache: 3M DMA ops/sec
```

---

## 13. Debugging SMMU Issues

### 13.1 SMMU Fault Decode

```
SMMU Fault registers (context bank):
  FSR (Fault Status Register):
    [9]  MULTI — Multiple errors recorded
    [8]  SS — Stall/Synchronous fault
    [7]  UUT — Unsupported Upstream Transaction
    [6]  AS — Address Size fault
    [5]  TLBLKF — TLB lock fault
    [4]  TLBMCF — TLB multi-hit fault
    [3]  EF — External fault
    [2]  PF — Permission fault
    [1]  AFF — Access flag fault
    [0]  TF — Translation fault

  FAR (Fault Address Register): IOVA that faulted
  FSYNR0 (Fault Syndrome 0):
    [4]    WNR (0=read fault, 1=write fault)
    [3:0]  TRANS — transaction type

Common fault scenarios:
  TF (Translation fault): IOVA not mapped in SMMU page tables
    → Missing iommu_map() call, or IOVA out of range
    
  PF (Permission fault): IOVA mapped but wrong permissions
    → DMA write to IOMMU_READ-only mapping
    
  TLBMCF (TLB multi-hit): Two entries match same IOVA
    → Software bug: double-mapped IOVA
    → CRITICAL: some SMMMUs panic on multi-hit
```

### 13.2 Debugging Tools

```bash
# Enable IOMMU fault reporting in Linux:
echo 1 > /sys/kernel/debug/iommu/smmuv2/fault_reporting

# Check SMMU domain mappings:
cat /sys/kernel/debug/iommu/smmuv2/*/mappings

# ARM SMMU v2 driver debug:
echo 8 > /proc/sys/kernel/printk   # max debug level
insmod arm_smmu.ko dyndbg=+p

# Trace IOMMU operations:
echo 1 > /sys/kernel/debug/tracing/events/iommu/enable
cat /sys/kernel/debug/tracing/trace | grep iommu

# Check if device is attached to IOMMU group:
ls /sys/bus/platform/devices/<dev>/iommu_group/

# SMMU page table dump (driver must implement):
cat /sys/kernel/debug/iommu/<smmu>/cb<N>/pgtable

# Check IOVA space:
cat /sys/kernel/debug/iommu/<domain>/iova

# dmesg for SMMU faults:
# [arm-smmu] context fault on cb0: IOVA=0x12340000, FSR=0x02 (PF), WNR=1
```

### 13.3 Systrace for IOMMU Performance

```bash
# Profile IOMMU map/unmap latency:
perf record -e iommu:map,iommu:unmap -a sleep 5
perf report

# Count SMMU TLB invalidations:
perf stat -e arm_smmu/tlbi_requests/ ./workload

# strace to see DMA operations from userspace perspective:
strace -e ioctl ./app 2>&1 | grep DMA_MAP
```

---

## Summary

| Component | Purpose | Key API |
|-----------|---------|---------|
| Stream ID | Identify DMA master | SoC-specific |
| Context Bank | Per-device IOVA space | arm-smmu CB registers |
| SMR/S2CR | SID → CB mapping | arm_smmu_write_sme() |
| iommu_domain | Abstract IOVA space | iommu_alloc_domain() |
| iommu_map() | IOVA → PA mapping | iommu_map() |
| IOVA allocator | Manage IOVA addresses | alloc_iova() |
| dma_map_single | Map buffer for DMA | dma_map_single() |
| SMMU TLB flush | Invalidate SMMU TLBs | arm_smmu_tlb_sync() |

---

**Cross-References:**
- Doc 01: ARM page table formats used in SMMU context banks
- Doc 05: Cache maintenance required alongside SMMU mapping
- Doc 06: Secure SMMU context banks (QSEE/TrustZone isolation)
- Doc 09: Stage-2 translation for hypervisor DMA isolation

---
**End of Document 7**
