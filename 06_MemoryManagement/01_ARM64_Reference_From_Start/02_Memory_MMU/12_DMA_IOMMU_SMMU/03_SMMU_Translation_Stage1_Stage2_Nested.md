# SMMU Translation: Stage 1, Stage 2, and Nested

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. SMMU Translation Overview

```
SMMU provides two stages of translation (mirrors CPU MMU):

Stage 1 (S1): IOVA → IPA (or PA if S2 not present)
  Controlled by: Context Descriptor (CD)
  Page tables: AArch64 format (same as CPU TTBR0/TTBR1)
  ASID: 16-bit, used for per-device or per-process isolation
  Purpose: per-device virtual address space
  
Stage 2 (S2): IPA → PA
  Controlled by: STE (Stream Table Entry)
  Page tables: AArch64 Stage 2 format (same as EL2 Stage 2)
  VMID: 16-bit, identifies the virtual machine
  Purpose: hypervisor memory isolation for passthrough devices

Without hypervisor (bare metal):
  Config = Stage 1 only (0b101)
  IOVA → [S1 walk] → PA

With hypervisor (KVM/ARM, Xen, OP-TEE):
  Config = Stage 1 + Stage 2 (0b111) for VM's devices
  Config = Stage 2 only (0b110) for host device not doing S1
  IOVA → [S1 walk: IPA] → [S2 walk: PA]
  
Bypass mode (Config = 0b100):
  No translation: DMA address = physical address
  Used for: trusted kernel-driver DMA (less isolation, higher throughput)
  
Abort mode (Config = 0b000):
  All DMA from this device → abort (return bus error)
  Used for: quarantining untrusted devices, probing phase
```

---

## 2. Stage 1 Translation Detail

```
Stage 1 page table: uses ARM64 AArch64 long-descriptor format

CD (Context Descriptor) equivalent of CPU's TTBR0/TCR_EL1:
  TTB0: Stage 1 translation table base (physical address)
  TCR: translation control (IA size, granule, shareability)
  ASID: address space identifier (tagged TLB entries)

SMMU S1 walk (4KB granule, 3 levels for 39-bit VA):

IOVA = 0x12345000:
  Bits [38:30] → PGD index = 0
  Bits [29:21] → PUD/PMD index = 9  
  Bits [20:12] → PTE index = 0x45
  Bits [11:0]  → offset = 0x000

  Walk:
    CD.TTB0 → PGD table (4KB page, 512 entries)
    PGD[0] → PUD table (allocated per demand)
    PUD[9] → PMD table
    PMD[0x45] = [PA[47:12] | access flag | prot | valid]
    PA = PMD entry's PA bits | 0x000

S1 fault types (same as CPU faults):
  Translation fault: no valid mapping at any level
  Access flag fault: AF bit not set (if hardware AF disabled)
  Permission fault: AP bits don't allow requested access (R/W/X)

Linux S1 page tables for SMMU:
  Each iommu_domain: io_pgtable_ops → arm_64_lpae_s1_alloc_pgtable()
  Format: ARM_64_LPAE_S1 (same page table format as CPU stage 1)
  Allocated dynamically: pgtable_ops->alloc_iova() + pgtable_ops->map()

SMMU ASID management:
  SMMU S1 TLB entries tagged with ASID (like CPU TLB)
  Different devices: different ASIDs → no cross-device TLB contamination
  SMMU v3: 16-bit ASID (64K unique address spaces)
  Invalidation: CMD_TLBI_NH_ASID invalidates all SMMU TLB entries with given ASID
```

---

## 3. Stage 2 Translation Detail

```
Stage 2: IPA → PA (I/O Physical Address → Physical Address)

Used in: VM with PCIe passthrough, ARM TrustZone (secure/non-secure)
VMID: identifies the VM (matches CPU's VMID in VTTBR_EL2)

STE Stage 2 fields:
  S2TTB: Stage 2 translation table base (format: AArch64 Stage 2)
  VTCR:  translation control (same fields as CPU VTCR_EL2):
    VTCR.T0SZ: IPA size (49 - T0SZ = IPA bits)
    VTCR.SL0: start level (Level 0/1/2)
    VTCR.TG0: granule (4KB=0, 16KB=1, 64KB=2)
    VTCR.SH0: shareability (01=IRGN, 11=ISH)

Stage 2 descriptor format (same as CPU EL2 Stage 2):
  [PA[47:N] | Contiguous | DBM | AF | SH | S2AP | MemAttr | NS | valid]
  
  S2AP[1:0]:
    00: No access
    01: Read only
    10: Write only (unusual)
    11: Read-write
  
  MemAttr[3:0]: Stage 2 memory attributes
    Device nGnRE, Normal WB, Normal NC, etc.

Combined S1+S2 walk:
  IOVA input → S1 walk gives IPA → S2 walk gives PA
  Both walks performed in hardware for every SMMU TLB miss
  Performance: S1+S2 adds extra page table walk latency per IOTLB miss
  SMMU v3 optimization: separate S1/S2 TLBs, combined S1+S2 TLB caching

KVM + SMMU integration (vSMMU):
  KVM (pKVM / VHE mode) configures SMMU Stage 2 tables
  When VM's device issues DMA: SMMU S2 translates IPA → PA
  IPA = VM's view of physical address (same as CPU Stage 2)
  PA = actual DRAM physical address

  Linux KVM: arm_smmu_domain_set_attr(DOMAIN_ATTR_NESTING) enables S1+S2
  Guest OS configures S1 (via device driver IOMMU operations)
  KVM hypervisor configures S2 (via iommu_domain with DOMAIN_ATTR_NESTING)
```

---

## 4. SMMU TLB and Cache Invalidation

```
SMMU internal caches:
  TLB: caches IOVA → PA translations (like CPU TLB)
  Configuration Cache (STLB): caches STE and CD entries
  
  When software changes SMMU page tables: MUST invalidate stale TLB entries!
  
  SMMUv3 invalidation sequence for Stage 1 VA range:
    1. Software updates page tables (clear PTE or change attributes)
    2. Write CMD_TLBI_NH_VA to CMDQ:
         StreamID = target device's StreamID
         ASID = target ASID (from CD)
         VA = IOVA to invalidate
         Range = size in pages
    3. Write CMD_SYNC to CMDQ:
         CS = 0 (no interrupt on completion)
         MSH = 11 (inner shareable — all SMMU observers must observe)
    4. Poll until CMDQ cons_ptr advances past CMD_SYNC
       OR: register interrupt for SYNC completion
    5. Now old IOTLB entries are gone — new page table visible to device

  Broadcast invalidation (all devices):
    CMD_TLBI_NH_ALL: invalidate all non-hypervisor TLB entries
    CMD_TLBI_S12_VMALL: invalidate all Stage 1+2 entries for VMID
    Used when unmapping a large region or destroying an iommu_domain

  Linux SMMUv3 invalidation code:
    arm_smmu_tlb_inv_context_s1(cookie):
      Queue: CMD_TLBI_NH_ASID(ASID) + CMD_SYNC
      arm_smmu_cmdq_issue_cmd_with_sync()

  SMMU TLB shootdown vs CPU TLB shootdown:
    CPU: TLBI *IS broadcast instructions → all CPUs (IP broadcast)
    SMMU: CMDQ commands → SMMU hardware processes them (IP-independent)
    Both require DSB/SYNC barriers to ensure completion before continuing
```

---

## 5. IOVA Management

```
IOVA (I/O Virtual Address): device's view of the address space

iova_domain: per-iommu_domain IOVA allocator
  Manages a range of device-visible addresses
  Allocates/frees IOVA ranges (similar to vmalloc for device space)
  
  IOVA range: typically matches device's DMA mask
    64-bit device: 0 to 2^64 (full range)
    32-bit device: 0 to 4GB
    Custom: set by dev->bus_dma_limit

iommu_dma_alloc_iova():
  iova_domain: red-black tree of free IOVA ranges
  alloc_iova(iovad, size, dma_limit): find free range, mark used
  Returns: IOVA (starting address in device's virtual address space)

iommu_map(domain, iova, phys, size, prot):
  Creates SMMU page table entry: IOVA → PA
  Updates SMMU's page tables (in normal RAM)
  Does NOT immediately flush SMMU TLB (done separately by caller)

iommu_unmap(domain, iova, size):
  Removes SMMU page table entry
  Does NOT flush TLB (caller must flush)

free_iova(iovad, iova):
  Returns IOVA range to free pool

Linux IOMMU DMA path (iommu_dma_map_page):
  1. alloc_iova(): find IOVA range
  2. iommu_map(): create IOVA → PA mapping in SMMU page tables
  3. arm_smmu_iotlb_sync(): post CMD_SYNC (ensure device sees new mapping)
  4. Return dma_addr = IOVA

Linux IOMMU DMA unmap (iommu_dma_unmap_page):
  1. iommu_unmap(): remove IOVA → PA from SMMU page tables
  2. iommu_iotlb_sync(): post CMD_TLBI + CMD_SYNC
  3. free_iova(): return IOVA to pool
```

---

## 6. Interview Questions & Answers

**Q1: Explain SMMU Stage 1 + Stage 2 combined translation. What is each stage responsible for?**

SMMU combined (nested) translation mirrors the CPU's two-stage translation in KVM:

**Stage 1 (device → IPA)**:
- Under control of the guest OS (or device driver in the guest)
- Same page table format as CPU Stage 1 (TTB0, LPAE long-descriptor)
- Maps IOVA (device's virtual addresses) → IPA (Intermediate Physical Address)
- IPA = what the guest OS thinks are physical addresses
- Guest OS: creates its own IOMMU domains, allocates IOVAs, programs device with IOVA

**Stage 2 (IPA → PA)**:
- Under control of the hypervisor (KVM/EL2)
- Same format as CPU Stage 2 (VTCR equivalent)
- Maps IPA → actual PA (host physical memory)
- Hypervisor ensures guest can only access its own allocated memory

**Combined walk**:
When device issues DMA to IOVA:
1. SMMU S1 walk: IOVA → IPA (using guest-provided page tables in CD)
2. SMMU S2 walk: IPA → PA (using hypervisor-provided S2 page tables in STE)
3. Each pointer fetch during S1 walk: the pointer PA itself goes through S2!
   This means S1 walk requires additional S2 walks for each level

Security guarantee: the hypervisor controls Stage 2. Guest OS can set S1 to any IPA, but IPA → PA only reaches memory the hypervisor allowed. This prevents a malicious VM from DMA'ing into another VM's memory or kernel memory.

**Q2: How does the SMMU protect against a compromised driver mapping all physical memory?**

Without IOMMU: a driver (or malicious kernel module) can call `dma_map_single()` with any physical address and program a PCIe device to DMA from that address. This allows reading kernel memory, other processes' memory, or hypervisor memory.

With SMMU: the DMA API (through the IOMMU framework) controls what IOVA → PA mappings exist. A malicious driver can only map physical pages that:
1. Were allocated through the proper DMA API (`dma_alloc_coherent`, `dma_map_single`)
2. Belong to the driver's own memory (GFP_KERNEL allocations owned by the driver)

The SMMU enforces this at hardware level: device DMA to any unmapped IOVA → translation fault → device receives bus error → DMA fails harmlessly.

Additional protection layers:
- IOMMU groups: PCIe devices that can see each other's DMA (due to PCIe ACS limitations) are in the same IOMMU group → must share the same domain → cannot isolate from each other (known limitation)
- Strict mode vs lazy mode: some IOMMU implementations start with device mapped to all memory (lazy) and restrict over time. ARM SMMU defaults to strict (initially abort all DMA)

---

## 7. Quick Reference

| SMMU Config | Stage 1 | Stage 2 | Use Case |
|---|---|---|---|
| 0b100 = Bypass | None | None | Trusted kernel DMA |
| 0b000 = Abort | None | None | Device quarantine |
| 0b101 = S1 only | IOVA → PA | None | Normal device |
| 0b110 = S2 only | None | IPA → PA | Hypervisor passthrough |
| 0b111 = S1+S2 | IOVA → IPA | IPA → PA | VM with nested IOMMU |

| SMMUv3 TLB Invalidation Command | What It Invalidates |
|---|---|
| CMD_TLBI_NH_VA | Single IOVA (Stage 1) |
| CMD_TLBI_NH_ASID | All IOVAs for one ASID |
| CMD_TLBI_NH_ALL | All Stage 1 entries |
| CMD_TLBI_S2_IPA | Single IPA (Stage 2) |
| CMD_TLBI_S12_VMALL | All S1+S2 for one VMID |
