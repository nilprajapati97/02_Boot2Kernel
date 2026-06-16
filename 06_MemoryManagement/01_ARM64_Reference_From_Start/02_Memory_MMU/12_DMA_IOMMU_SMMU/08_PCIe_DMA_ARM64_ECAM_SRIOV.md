# PCIe DMA on ARM64: Root Complex, SR-IOV, and SMMU Integration

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. PCIe Fundamentals on ARM64

```
PCIe (Peripheral Component Interconnect Express):
  Serial point-to-point interconnect
  Hierarchy: Root Complex → Switches → Endpoints
  
ARM64 PCIe topology:
  ┌────────────────────────────────────┐
  │  ARM64 SoC                         │
  │  ┌──────────┐    ┌──────────────┐  │
  │  │ CPU      │    │ PCIe Root    │  │
  │  │ Clusters │    │ Complex (RC) │  │
  │  └────┬─────┘    └──────┬───────┘  │
  │       │                 │           │
  │  ┌────▼─────────────────▼───────┐  │
  │  │     Coherent Interconnect    │  │
  │  │     (CMN-700 / CCI-550)      │  │
  │  └────────────────┬─────────────┘  │
  │  ┌─────────────────▼────────────┐  │
  │  │   SMMU (ARM SMMUv3)          │  │
  │  └─────────────────┬────────────┘  │
  │                    │               │
  └────────────────────┼───────────────┘
                       │ PCIe lanes
                   ┌───▼────┐
                   │ Switch │
                   └──┬──┬──┘
                      │  │
                 ┌────┘  └────┐
                 │NVMe        │NIC
                 │Endpoint    │Endpoint
                 └────────────┘

PCIe Transaction Layer Packets (TLPs):
  Every DMA read/write from device → TLP to memory
  TLP header: [Requester ID (RID)] + [address] + [length] + [tag]
  
  Requester ID (RID) = Bus:Device:Function (16 bits total):
    Bus [15:8]:     PCIe bus number (0-255)
    Device [7:3]:   Device number (0-31)
    Function [2:0]: Function number (0-7)
  
  SMMU uses RID as StreamID:
    NVMe at Bus=1, Dev=0, Fun=0: RID = 0x0100 = StreamID 256
    NIC at Bus=1, Dev=1, Fun=0: RID = 0x0108 = StreamID 264
    
    SMMU STE lookup: strtab[StreamID].Config
    Each device: independent translation domain
```

---

## 2. PCIe ECAM on ARM64

```
ECAM (Enhanced Configuration Access Mechanism):
  Standard PCIe configuration space access method
  Maps PCIe config space into CPU physical address space
  
  Formula: config_addr = ecam_base + (bus << 20) | (dev << 15) | (fn << 12)
  Each PCIe function: 4KB config space
  Full ECAM region: 256MB for all 256 buses (256 × 256 × 8 × 4KB = 256MB!)
  
ARM64 ECAM requirements:
  UEFI + ACPI:
    MCFG table: lists ECAM base addresses per PCIe segment
    Each segment = one Root Complex
    Linux: pci_acpi_scan_root() → pci_ecam_create()
    ioremap(ecam_base, ECAM_SIZE): maps 256MB as device memory (nGnRE)
  
  Device Tree:
    reg = <0x40000000 0x10000000>: ECAM range in DT
    linux,pci-domain: PCIe domain number
    Linux: pci_host_probe() → of_pci_bus_find_domain_nr()
  
  Accessing PCIe config space:
    ecam_virt + (bus << 20) + (dev << 15) + (fn << 12) + offset
    writel(0x04, ecam_virt + 0x0100000 + 0x04): write Bus=0,Dev=2,Fn=0 Command reg
    readl(ecam_virt + ...): read any BAR or capability

ARM64-specific PCIe issues:
  1. Memory-mapped PCIe config: nGnRnE (non-gathering, non-reordering, non-Early ack)
     Some ARM64 SoCs: ECAM region requires Device nGnRnE
     Reason: PCIe config reads are side-effectful; reordering dangerous
  
  2. ECAM alignment: ARM64 requires 4KB natural alignment for config spaces
     Non-compliant hardware: ECAM not aligned → Linux quirks needed
  
  3. PCIe 64-bit BARs: device asks for 64-bit address space
     ioremap(): map as Device nGnRE (32-bit) or Device nGnRnE (strict)
     ARM64: MAIR.Attr = 0b00000000 for Device nGnRnE
```

---

## 3. PCIe ACS (Access Control Services)

```
ACS: PCIe capability to control peer-to-peer traffic routing

ACS capabilities:
  ACS Source Validation (ACS_SV): validate requester ID
  ACS Translation Blocking (ACS_TB): block untranslated DMA
  ACS P2P Request Redirect (ACS_RR): redirect P2P DMA through Root Complex (and IOMMU)
  ACS P2P Completion Redirect (ACS_CR): redirect P2P completions
  ACS Direct Translated P2P (ACS_DT): allow ATS-translated P2P
  ACS I/O Request Blocking (ACS_IO): block I/O transactions
  
  Most important for IOMMU isolation: ACS_RR (P2P Request Redirect)
    Without ACS_RR: two PCIe devices on same switch can DMA to each other
                    WITHOUT going through the IOMMU → bypasses all isolation!
    With ACS_RR: peer-to-peer DMA goes through Root Complex → SMMU intercepts → checks translation

Linux ACS handling:
  pci_enable_acs(pdev): enable all ACS features the device supports
  pci_acs_enabled(pdev, mask): check if specific ACS bits enabled
  pci_acs_path_enabled(dev, upstream, mask): check ACS along entire path
  
  IOMMU grouping with ACS:
    pci_device_group() →
      pci_acs_path_enabled(dev, NULL, REQ_ACS_FLAGS)?
        YES: device can be isolated → own IOMMU group
        NO:  must be grouped with other devices on same switch
    
    REQ_ACS_FLAGS = ACS_SV | ACS_RR | ACS_CR | ACS_UF

ARM64 servers and ACS:
  AWS Graviton 3: PCIe Gen5 root complex, ACS supported per-device
  Ampere Altra: PCIe Gen4, ACS via RC for fine-grained VFIO
  Qualcomm Data Center: SMMU + ACS for per-VF isolation
```

---

## 4. SR-IOV (Single Root I/O Virtualization)

```
SR-IOV: PCIe capability to create multiple virtual PCIe functions from one device

Physical Function (PF): full PCIe device (NIC, NVMe, etc.)
Virtual Function (VF): lightweight PCIe function sharing PF hardware
  Each VF: own BAR, own DMA queue, own PCIe config space
  VFs created by PF driver: pci_enable_sriov(pdev, num_vfs)
  
SR-IOV + SMMU (device passthrough to VMs):
  PF: has IOMMU group, managed by host kernel
  VF 0: own IOMMU group (if ACS enabled), passed through to VM 0
  VF 1: own IOMMU group, passed through to VM 1
  
  Each VF: own StreamID (RID = Bus:Device:VF_Function)
           own SMMU Stream Table Entry
           own Stage 2 page tables (VM's IPA → PA)
  
  VM 0 programs VF 0 for DMA:
    → VF 0 issues TLP with RID = VF_0_ID
    → SMMU: STE[VF_0_StreamID].S2TTB = VM0's stage2 page tables
    → SMMU Stage 2 walk: IPA → VM0_PA (only VM0's memory accessible)
    → VM 1's memory: not accessible via VF 0 (stage 2 doesn't map it)

SR-IOV VF assignment flow (KVM + VFIO):
  1. Host: pci_enable_sriov(pf, N): create N VFs
  2. Host: unbind VF from host driver, bind to vfio-pci
  3. Host: configure SMMU Stage 2 for VF: iommu_attach_device(vm_domain, vf_dev)
  4. Host QEMU: vfio_group_get_device_fd(vf_device)
  5. KVM: map VM physical address space → host physical
  6. SMMU: now translates VF DMA (IPA from VM → PA via stage 2)
  
ARM64 SR-IOV StreamID:
  PCIe ARI (Alternative Routing-ID Interpretation) extends function number
  With ARI: 8-bit function number per device → up to 256 VFs per device
  StreamID = RID with ARI: Bus[15:8] + Device[7:3] + ExtFunc[2:0|extended]
  In practice: VF 0 = BDF 00:00.0, VF 1 = 00:00.1, ..., VF 255 = 00:00.255
  SMMU: 256 separate STEs for 256 VFs (independent isolation)
```

---

## 5. PCIe DMA Buffer Lifecycle on ARM64

```
Full PCIe NVMe DMA read on ARM64 with SMMU:

1. Kernel allocates I/O buffer:
   bio_alloc() → alloc_pages() → pages at PA = 0x200000000 (9GB range)

2. DMA map:
   dma_map_page(nvme_dev, page, offset, len, DMA_FROM_DEVICE):
   → iommu_dma_map_page():
       alloc_iova(iovad, len): returns IOVA = 0x10000 (32-bit range)
       iommu_map(domain, 0x10000, 0x200000000, len, READ|WRITE)
       → arm_smmu_map_pages(): write SMMU PTE: 0x10000 → 0x200000000
       post CMD_SYNC: ensure SMMU sees new mapping
   → returns dma_addr = 0x10000

3. Program NVMe controller:
   nvme_prp_list[0] = dma_addr = 0x10000
   nvme_submit_io_cmd(prp_list, READ, lba, count)
   MMIO write: doorbell register → NVMe starts DMA

4. NVMe hardware DMA:
   NVMe controller issues PCIe Memory Read TLP:
     [RID=Bus:Dev:Fn] [addr=0x10000] [len=4096] [read]
   PCIe → SMMU
   SMMU: lookup STE[RID] → S1 domain → CD → page walk
         0x10000 → 0x200000000 (found in IOTLB or fresh walk)
   SMMU: DMA request goes to CMN-700 interconnect
   CMN-700: routes to DRAM controller
   DRAM: returns data to NVMe DMA engine

5. NVMe signals completion:
   NVMe posts completion to CQ (DMA write to completion queue PA)
   CQ: also mapped via SMMU
   NVMe issues MSI-X interrupt: IRQ to CPU

6. CPU processes completion:
   nvme_irq() → blk_complete_request()
   dma_unmap_page():
     iommu_unmap(domain, 0x10000, len)
     arm_smmu_iotlb_sync()  // CMD_TLBI + CMD_SYNC
     free_iova(iovad, 0x10000)
   
   If non-coherent bus: arch_sync_dma_for_cpu() → DC IVAC + DSB
   CPU reads I/O buffer: gets NVMe data

7. I/O complete.
```

---

## 6. Interview Questions & Answers

**Q1: How does the SMMU identify which DMA transaction belongs to which VM when multiple VMs share the same PCIe NIC?**

With SR-IOV, each VM gets its own Virtual Function (VF). Each VF has a unique PCIe Requester ID (RID = Bus:Device:Function). The SMMU maps each RID to a unique StreamID, and each StreamID has its own Stream Table Entry (STE).

For VM isolation:
- VF 0 → STE[RID_VF0]: Stage 2 page tables = VM 0's IPA space
- VF 1 → STE[RID_VF1]: Stage 2 page tables = VM 1's IPA space

When VF 0 issues a DMA transaction, its RID appears in the PCIe TLP header. The SMMU extracts this RID as the StreamID, looks up STE[RID_VF0], performs Stage 2 translation using VM 0's page tables. Even if VF 0 tries to DMA to an IPA that belongs to VM 1, the Stage 2 walk would fail (translation fault) because VM 0's stage 2 doesn't map VM 1's memory.

This gives complete hardware-enforced memory isolation between VMs sharing the same physical PCIe device.

**Q2: What is PCIe ECAM and how does Linux access PCIe configuration space on ARM64?**

ECAM (Enhanced Configuration Access Mechanism) is the standard mechanism that maps the entire PCIe configuration space into a flat physical address range on the CPU. The formula is:

`config_addr = ecam_base + (bus << 20) | (device << 15) | (function << 12) + register_offset`

Each PCIe function gets exactly 4KB of configuration space. With 256 buses × 32 devices × 8 functions × 4KB = 256MB total ECAM region per PCIe segment.

On ARM64:
1. Firmware (UEFI) provides ECAM base address via ACPI MCFG table (or via DT `reg` property)
2. Linux kernel calls `ioremap(ecam_base, 256MB)` to map as Device nGnRE or nGnRnE memory
3. PCI config reads/writes use `readl()/writel()` to this virtual address range
4. For example: `readl(ecam_virt + (1 << 20) + (0 << 15) + (0 << 12) + 0x10)` reads BAR0 of Bus=1, Dev=0, Fn=0

The key constraint on ARM64: PCIe config space uses Device memory type (MAIR = 0b00000000, nGnRnE) to prevent speculative accesses to PCIe config registers, which may have side effects.

---

## 7. Quick Reference

| PCIe Concept | ARM64 Specifics |
|---|---|
| Requester ID (RID) | = SMMU StreamID for PCIe devices |
| ECAM base | From ACPI MCFG or DT, mapped as Device nGnRnE |
| ACS | Controls P2P traffic routing; affects IOMMU groups |
| SR-IOV VF | Each VF = own RID = own SMMU STE = own isolation domain |
| ATS | Device-side IOTLB; requires SMMU ATS support (STE.EATS) |

| SMMU StreamID Source | How StreamID Is Assigned |
|---|---|
| PCIe endpoint | RID = Bus[15:8] + Dev[7:3] + Fn[2:0] |
| PCIe VF (SR-IOV) | RID with extended function (ARI) |
| Platform device | Static assignment in DTS (`iommus =` property) |
| PASID (SVM) | SubstreamID = PASID (20-bit) per PCIe function |
