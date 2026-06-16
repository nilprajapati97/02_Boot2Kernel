# ARM SMMU Architecture: v1, v2, and v3

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
SMMU (System Memory Management Unit): ARM's IOMMU (I/O MMU)
  Purpose: virtual address translation for DMA masters (devices)
  
  Why SMMU?
    Security: without SMMU, any DMA-capable device (PCIe endpoint)
              can read/write ANY physical memory — catastrophic!
              With SMMU: device can ONLY access its allocated IOVA ranges
    
    Isolation: multiple VMs on one server — each VM's devices must
               only access its own VM's memory (not other VMs)
               SMMU Stage 2 translates IOVA → IPA → PA for VMs
    
    Addressing: 32-bit DMA devices on 64-bit ARM systems
                SMMU maps a 32-bit IOVA → 64-bit PA (above 4GB)
                No bounce buffers needed!
    
    IOVA management: driver works with IOVA space, kernel maps IOVA → PA

SMMU versions:
  SMMUv1: ARM System Memory Management Unit v1 (early)
           Stream-based translation
           Simple page tables (similar to ARMv7 short-descriptor)
  
  SMMUv2: Most widely deployed (many ARM SoCs, AWS Graviton 1)
           Long-descriptor page tables (ARMv8 AArch32 LPAE format)
           Stream match registers for device identification
           Stage 1 + Stage 2 translation
           SMMU context banks (per-device translation contexts)
  
  SMMUv3: Modern, scalable (Neoverse N1/N2, Graviton 3, Apple M1, etc.)
           AArch64 page table format (same as CPU's MMU)
           Stream Table (flat or 2-level) for device identification
           Command Queue / Event Queue (ring buffers)
           PRI (Page Request Interface) for SVM
           PCIe ATS support (TLB in device)
           Much higher performance than v2
```

---

## 2. SMMU v3 Architecture

```
SMMUv3 key structures:

1. Stream Table Entry (STE): per-device configuration
   ┌──────────────────────────────────────────────┐
   │  STE (64 bytes = 8 DWORDs)                   │
   │  Word 0:                                       │
   │    Valid[0]: 1 = valid entry                  │
   │    Config[4:1]:                               │
   │      b000 = Abort (return error to device)    │
   │      b100 = Bypass (no translation)           │
   │      b101 = Stage 1 only                      │
   │      b110 = Stage 2 only                      │
   │      b111 = Stage 1 + Stage 2                 │
   │    S1FMT[5]: format of S1 pointer             │
   │  Word 1:                                       │
   │    S1ContextPtr[51:4]: pointer to CD (s1 ctx) │
   │  Word 2-3:                                     │
   │    S2TTB: Stage 2 Translation Table Base      │
   │    VTCR: S2 translation control               │
   │  Word 4-7: other configuration                │
   └──────────────────────────────────────────────┘

2. Context Descriptor (CD): Stage 1 translation config (per address space)
   ┌──────────────────────────────────────────────┐
   │  CD (64 bytes)                               │
   │  ASID[31:16]: address space ID               │
   │  V[0]: valid bit                             │
   │  AA64[9]: AArch64 page tables                │
   │  TTB0[51:4]: Stage 1 translation table base  │
   │  TCR: translation control (like ARM64 TCR_EL1)│
   │  MAIRs: memory attribute registers           │
   └──────────────────────────────────────────────┘

3. Stream Table: maps device StreamID → STE
   StreamID: how the SMMU identifies the source device
     For PCIe: [Requester ID] = [Bus:Device:Function] = 16 bits
     For platform devices: assigned in device tree
   
   Two formats:
     Linear: flat array indexed by StreamID directly
             Fast O(1) lookup, wastes memory for sparse StreamIDs
     2-Level: L1 (256-entry pointer array) → L2 (leaf STEs)
              More compact for sparse StreamIDs

4. Command Queue (CMDQ): software → SMMU commands (ring buffer)
   Commands:
     CMD_CFGI_STE:    Invalidate STE cache (after modifying STE)
     CMD_CFGI_CD:     Invalidate CD cache (after modifying CD)
     CMD_TLBI_NH_VA:  Invalidate TLB by VA (SMMU Stage 1, all ASIDs)
     CMD_TLBI_NH_ASID: Invalidate TLB by ASID
     CMD_TLBI_S2_IPA:  Invalidate Stage 2 TLB by IPA
     CMD_SYNC:         Synchronize (wait for all prior commands to complete)
   
   Software: write command to CMDQ.base + prod_ptr
   SMMU: reads from CMDQ.base + cons_ptr
   Interrupt: when cons_ptr advances (command processed)

5. Event Queue (EVTQ): SMMU → software events (ring buffer)
   Events:
     F_WALK_EABT:       External abort during page table walk
     F_TRANSLATION:     Translation fault (no valid mapping)
     F_ADDR_SIZE:       Address outside configured range
     F_ACCESS:          Permission fault
     F_TLB_CONFLICT:    TLB conflict
```

---

## 3. SMMU v3 Translation Flow

```
Device issues DMA read to address 0x10000000:

1. SMMU receives DMA request: (StreamID=0x0100, address=0x10000000)

2. STE lookup:
   StreamID = 0x0100 (PCIe RID for Bus=0, Device=2, Function=0)
   Index into Stream Table: strtab.base + (0x0100 * 64)
   Read STE, check Config = 0b101 (Stage 1 only)

3. CD lookup (Stage 1):
   STE.S1ContextPtr → CD array
   SubstreamID 0 → CD[0]
   Read CD: TTB0 = 0xFFFFF000B0000000 (this device's page tables)
             ASID = 0x42

4. Stage 1 walk:
   Same as CPU MMU walk!
   0x10000000 = 0b_0000...0001_0000_0000_0000_0000_0000_0000
   PGD[0] → PUD[0] → PMD[1] → PTE[0] = PA 0x50000000 | Normal | RW
   
   Result: translate 0x10000000 → PA 0x50000000

5. Forward to memory: device reads from physical 0x50000000

6. TLB caches: SMMU has its own TLB (IOTLB)
   Caches: StreamID + ASID + IOVA → PA mapping
   Invalidation: when Linux changes SMMU page tables:
     Post CMD_TLBI_NH_ASID to CMDQ
     Post CMD_SYNC to CMDQ
     Wait for SYNC completion

ARM64 SMMUv3 driver: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c
```

---

## 4. SMMU v3 Stage 2 (Hypervisor)

```
Stage 2 translation: used for virtual machine isolation
  Physical device (e.g., NVMe) assigned to VM via PCIe passthrough (VFIO)
  Device DMA uses IPA (Intermediate Physical Address = VM's "physical address")
  SMMU Stage 2: IPA → PA (using hypervisor's page tables)
  
  This ensures:
    VM's device can only DMA to VM's physical pages
    Other VMs' memory is protected

STE with Stage 2:
  STE.Config = 0b110 (Stage 2 only)
  STE.S2TTB: Stage 2 translation table (similar to CPU VTTBR_EL2)
  STE.VTCR: VTCR_EL2-equivalent for SMMU

Stage 2 page table format:
  Same as CPU EL2 Stage 2 (AArch64 long-descriptor format)
  VTCR.SL0: start level (0=48-bit, 1=44-bit IPA)
  Each SMMU S2 page table entry:
    [PA[47:12] | AF | SH | MemAttr | R/W | valid]

Nested translation (Stage 1 + Stage 2):
  STE.Config = 0b111
  Guest driver writes IOVA → S1 page tables (CD.TTB0)
  Host hypervisor maps IPA → PA in S2 tables (STE.S2TTB)
  
  SMMU performs TWO-level walk:
    IOVA → [S1 walk: IPA] → [S2 walk: PA]
  
  This is called "nested paging" for DMA — mirrors CPU's Stage1+Stage2
```

---

## 5. Linux IOMMU Framework

```
Linux IOMMU/SMMU driver stack:
  
  Device Driver (e.g., NVMe)
       │ dma_map_single() / dma_alloc_coherent()
       ▼
  DMA API (kernel/dma/mapping.c)
       │ ops→map_page() / ops→alloc()
       ▼
  IOMMU DMA layer (drivers/iommu/dma-iommu.c)
       │ iommu_map(domain, iova, pa, size, prot)
       ▼
  ARM SMMU v3 driver (drivers/iommu/arm/arm-smmu-v3.c)
       │ arm_smmu_map_pages() → writes SMMU page tables
       ▼
  SMMU hardware: stream table, context descriptors, command queue

Key data structures:
  struct iommu_domain: per-device (or per-group) translation domain
    Contains: IOVA allocator (iova_domain), ARM64 page tables for SMMU
    
  struct iommu_group: group of devices that MUST share same domain
    (PCIe ACS non-compliant: devices on same PCIe switch = same group)
    
  struct iommu_ops: per-architecture SMMU operations
    .domain_alloc(): create translation domain
    .attach_dev(): attach device to domain (write STE/CD in SMMU)
    .map_pages(): map IOVA → PA in domain's page tables
    .unmap_pages(): remove mapping
    .flush_iotlb_all(): flush SMMU IOTLB for domain
    .iova_to_phys(): translate IOVA → PA (for debugging)
```

---

## 6. Interview Questions & Answers

**Q1: What is a StreamID in SMMU and how does the SMMU identify which device issued a DMA request?**

A StreamID is a hardware-assigned identifier that uniquely identifies the source of a DMA transaction on the system bus. The SMMU uses the StreamID to look up the corresponding Stream Table Entry (STE), which contains the translation configuration for that device.

**For PCIe devices**: StreamID = Requester ID (RID) = 16-bit field encoding [Bus (8 bits), Device (5 bits), Function (3 bits)]. Every PCIe transaction carries the RID of the originating device. The SMMU sees this RID as the StreamID.

**For platform devices** (integrated peripherals on ARM SoC): StreamID is assigned statically in the device tree. Example in DTS:
```dts
eMMC@48000000 {
    iommus = <&smmu0 0x80>;  // StreamID = 0x80
};
```

**For PASID** (PCIe ATS, SVM): the full 20-bit PASID is appended: SubstreamID = PASID. The CD array in the STE is indexed by SubstreamID, allowing per-process isolation within one device.

The two-level stream table efficiently handles sparse StreamID spaces: L1 has 256 pointers to L2 pages; each L2 page covers 256 StreamIDs. Only L2 pages for actually-present devices need to be allocated.

**Q2: What is the difference between SMMUv2 and SMMUv3 performance-wise?**

SMMUv2 uses a **legacy register-based interface**: every SMMU configuration change (TLB invalidation, context configuration) requires writing to separate MMIO registers, which involves device-memory writes (expensive, serialized, non-pipelined).

SMMUv3 uses **queue-based interface** (CMDQ, EVTQ):
- Commands are written to a ring buffer in normal (cacheable) memory
- Software can batch multiple commands (many TLBIs, then one SYNC)
- SMMU hardware reads from the queue independently → overlapped execution
- Commands complete in parallel with software posting more commands

Performance differences:
- SMMUv3 CMDQ batching: 10–100× more TLBIs per second
- SMMUv3 IOTLB: larger, multi-level, more entries
- SMMUv3 page table format: AArch64 format (same as CPU) → simpler SW, potential hardware optimization
- SMMUv3 PCI ATS support: device has its own IOTLB → fewer SMMU walks per transfer

In practice, SMMUv3 adds <10% DMA overhead vs no SMMU on modern hardware. SMMUv2 overhead could be 20–30% for I/O-intensive workloads.

---

## 7. Quick Reference

| Feature | SMMUv1 | SMMUv2 | SMMUv3 |
|---|---|---|---|
| Page table format | ARMv7 | LPAE (ARMv8 AArch32) | AArch64 (same as CPU) |
| Stream identification | Stream match | Stream match registers | Stream Table (flat/2-level) |
| Configuration interface | MMIO registers | MMIO registers | Command Queue (ring buffer) |
| Stage 2 | No | Yes | Yes |
| PCIe ATS | No | No | Yes |
| PASID/SVM | No | No | Yes |
| Error reporting | Interrupts | Interrupts | Event Queue |

| SMMUv3 Command | Purpose |
|---|---|
| CMD_CFGI_STE | Invalidate STE cache |
| CMD_CFGI_CD | Invalidate CD cache |
| CMD_TLBI_NH_ALL | Invalidate all TLB entries |
| CMD_TLBI_NH_ASID | Invalidate by ASID |
| CMD_TLBI_NH_VA | Invalidate by VA |
| CMD_TLBI_S2_IPA | Invalidate Stage 2 by IPA |
| CMD_SYNC | Wait for all prior commands |
