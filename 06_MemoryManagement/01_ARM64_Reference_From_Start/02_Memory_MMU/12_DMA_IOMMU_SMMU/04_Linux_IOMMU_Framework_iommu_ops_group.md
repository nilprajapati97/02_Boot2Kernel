# Linux IOMMU Framework: iommu_ops, iommu_group, iommu_domain

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Linux IOMMU framework: hardware-agnostic abstraction for IOMMUs

Supports multiple IOMMU hardware:
  ARM SMMU v1/v2/v3: arm-smmu.c, arm-smmu-v3.c
  Intel VT-d:        intel-iommu.c
  AMD-Vi:            amd_iommu.c
  ARM Mali GPU MMU:  mali-gpu iommu
  Qualcomm IOMMU:    qcom_iommu.c
  Apple DART:        apple-dart.c

Three-layer architecture:
  
  Device Driver (e.g., NVMe, Ethernet)
      │ dma_map_single() / dma_alloc_coherent()
      ▼
  DMA API (arch-neutral: kernel/dma/)
      │ if IOMMU present: delegates to iommu_dma_ops
      ▼
  IOMMU Core (drivers/iommu/iommu.c)
      │ iommu_map() / iommu_unmap()
      ▼
  Hardware Driver (drivers/iommu/arm/arm-smmu-v3.c)
      │ arm_smmu_map_pages() / arm_smmu_tlb_inv*()
      ▼
  SMMU Hardware
  
Key IOMMU framework concepts:
  iommu_domain: translation domain (set of IOVA → PA mappings)
                one domain per device (or per-group for grouped devices)
  iommu_group:  set of devices that MUST share the same domain
                (because they cannot be isolated from each other)
  iommu_ops:    hardware-specific implementation function table
  iova_domain:  IOVA allocator for a domain
```

---

## 2. iommu_domain

```c
/* include/linux/iommu.h */

struct iommu_domain {
    unsigned type;           // IOMMU_DOMAIN_UNMANAGED, IOMMU_DOMAIN_DMA, etc.
    const struct iommu_ops *ops;  // hardware operations
    unsigned long pgsize_bitmap;  // supported page sizes
    iommu_fault_handler_t handler; // fault callback
    struct iommu_domain_geometry geometry; // address space limits
    
    // For DMA-API backed domains:
    struct iommu_dma_cookie *iova_cookie; // IOVA allocator state
};

Domain types:
  IOMMU_DOMAIN_BLOCKED:    All DMA blocked (default for new devices)
  IOMMU_DOMAIN_IDENTITY:   1:1 mapping (IOVA = PA, no translation)
  IOMMU_DOMAIN_UNMANAGED:  Software manages mappings (VFIO, custom drivers)
  IOMMU_DOMAIN_DMA:        Managed by DMA API (most common)
  IOMMU_DOMAIN_SVA:        Shared Virtual Addressing (PASID-based)

iommu_domain lifecycle:
  Creation:
    iommu_domain_alloc(bus): allocate domain
    → calls ops->domain_alloc(type)
    → ARM SMMU: allocates page tables, ASID, IOVA domain
  
  Attach device:
    iommu_attach_device(domain, dev):
    → ops->attach_dev(domain, dev)
    → ARM SMMU v3: sets device's STE to point to domain's CD/page tables
                    writes STE to SMMU memory, posts CMD_CFGI_STE to CMDQ
  
  Map pages:
    iommu_map(domain, iova, paddr, size, prot):
    → ops->map_pages(domain, iova, paddr, pgsize, prot)
    → ARM SMMU: writes PTEs to domain's page tables
                Does NOT flush IOTLB here (caller batches flushes)
  
  Unmap + flush:
    iommu_unmap(domain, iova, size)
    iommu_iotlb_sync(domain, &gather)  // post TLBI + CMD_SYNC
  
  Detach + destroy:
    iommu_detach_device(domain, dev): write STE to Abort mode
    iommu_domain_free(domain): free page tables, return ASID
```

---

## 3. iommu_group

```c
/* drivers/iommu/iommu.c */

struct iommu_group {
    struct kobject kobj;
    struct kobject *devices_kobj;
    struct list_head devices;    // list of devices in this group
    struct mutex mutex;
    struct iommu_domain *domain; // current domain for all devices in group
    char *name;
    int id;
    struct iommu_domain *default_domain;  // auto-created for DMA API
    struct iommu_domain *blocked_domain;  // blocked (all DMA → abort)
};

When are devices in the same group?
  1. PCIe ACS (Access Control Services) not supported:
     PCIe switch or root port without ACS cannot guarantee that
     peer-to-peer DMA between devices is intercepted by IOMMU.
     All devices behind such a switch must be in same group.
  
  2. Platform devices sharing an SMMU stream context bank (SMMU v2):
     If two devices share the same context bank in SMMU v2,
     they cannot be independently isolated.
  
  3. Explicit grouping via DTS:
     iommu-group property in device tree
  
  4. Default: each device is its own group (if fully isolated)

IOMMU group in VFIO (VM device passthrough):
  VFIO requires: entire iommu_group must be passed through to VM
  Reason: all devices in group share a domain → must all be in VM
  
  If PCIe switch has 3 devices (no ACS): all 3 in same IOMMU group
  → VFIO: must pass ALL 3 to the VM, or none
  
  This is why servers use PCIe switches with ACS for fine-grained VFIO passthrough

Managing groups:
  iommu_group_add_device(group, dev): add device to group
  iommu_group_get_for_dev(dev): find/create group for device
  iommu_group_for_each_dev(group, fn): iterate all devices
```

---

## 4. iommu_ops (SMMU v3 Implementation)

```c
/* drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c */

static const struct iommu_ops arm_smmu_ops = {
    .capable            = arm_smmu_capable,
    .domain_alloc       = arm_smmu_domain_alloc,
    .probe_device       = arm_smmu_probe_device,
    .release_device     = arm_smmu_release_device,
    .device_group       = arm_smmu_device_group,
    .attach_dev         = arm_smmu_attach_dev,
    .map_pages          = arm_smmu_map_pages,
    .unmap_pages        = arm_smmu_unmap_pages,
    .flush_iotlb_all    = arm_smmu_flush_iotlb_all,
    .iotlb_sync         = arm_smmu_iotlb_sync,
    .iova_to_phys       = arm_smmu_iova_to_phys,
    .enable_nesting     = arm_smmu_enable_nesting,
    .set_pgtable_quirks = arm_smmu_set_pgtable_quirks,
};

arm_smmu_map_pages():
  → io_pgtable_ops->map_pages():
      ARM64 LPAE S1 page table operations
      Write PTEs: arm_lpae_set_pte()
      Memory barrier: dsb(ishst) // ensure PTE write visible before next step
  → No TLB flush here (batched separately)

arm_smmu_iotlb_sync(domain, &gather):
  → arm_smmu_cmdq_issue_cmd(smmu, CMD_TLBI_NH_ASID(asid)):
      Write command to CMDQ head
  → arm_smmu_cmdq_issue_cmd(smmu, CMD_SYNC):
      Write SYNC command
  → arm_smmu_cmdq_poll_until_consumed():
      Poll CMDQ cons_ptr until past SYNC
      → SMMU TLB is now clean

arm_smmu_attach_dev(domain, dev):
  → arm_smmu_write_ctx_desc(smmu_domain, ssid, cd):
      Write Context Descriptor to STE.S1ContextPtr memory
      Post CMD_CFGI_CD(sid, ssid, leaf) to CMDQ
  → arm_smmu_write_strtab_ent(master, sid, ste):
      Write STE to stream table
      Post CMD_CFGI_STE(sid, leaf) to CMDQ
  → arm_smmu_cmdq_issue_cmd(CMD_SYNC):
      Wait for configuration to take effect
```

---

## 5. IOMMU Fault Handling

```
SMMU fault types (SMMUv3 Event Queue events):
  F_UUT (F_UNSUPPORTED_UPSTREAM_TRANSACTION): unsupported transaction type
  F_BAD_ATS_TREQ: bad ATS translation request
  F_TRANSLATION: no valid translation (access to unmapped IOVA)
  F_ADDR_SIZE: address out of configured range
  F_ACCESS: permission denied (write to read-only, or no access)
  F_WALK_EABT: external abort during page table walk
  F_TLB_CONFLICT: TLB conflict (concurrent access during invalidation)

Event Queue processing:
  SMMU writes fault event to EVTQ ring buffer
  Triggers IRQ: arm_smmu_evtq_handler()
    Read event from EVTQ
    Decode: StreamID, SubstreamID, IOVA, fault type
    Find device: smmu_master by StreamID
    Call registered fault handler: iommu_report_device_fault()
    → Driver can handle page fault (for SVM/PASID use case)
    → Or: SMMU returns bus error to device (default)

IOMMU fault handler registration:
  iommu_register_device_fault_handler(dev, handler, data):
    Registered per-device
    Called with: struct iommu_fault {
        fault_type, addr, pasid, prot_flags, ...
    }
  
  For SVM (Shared Virtual Memory): handler can fault-in the page:
    1. fault_handler() called: IOVA = userspace virtual address
    2. Handle page fault: do_page_fault(IOVA) → alloc/mmap page
    3. Map new page in SMMU domain: iommu_map(domain, IOVA, new_PA, ...)
    4. Respond to SMMU: SMMU.PRI_Queue → PRI_RESP(success)
    5. Device retries DMA → SMMU TLB miss → page table walk → success!

ARM64 SMMU fault stats:
  /sys/kernel/debug/arm-smmu-v3/: per-SMMU debug interface
  /sys/bus/platform/drivers/arm-smmu-v3/: driver binding
```

---

## 6. Interview Questions & Answers

**Q1: What is an IOMMU group and why can't VFIO pass through just a single device from a group?**

An IOMMU group is a set of devices that CANNOT be isolated from each other by the IOMMU hardware. Devices in the same group see each other's DMA — if one device were given to a VM and others stayed in the host, the VM's device could DMA into memory that the host's devices use, breaking isolation.

The most common reason for grouping: **PCIe without ACS (Access Control Services)**. ACS is a PCIe capability that makes the root complex/switch forward peer-to-peer transactions through the IOMMU instead of routing them directly between devices. Without ACS, Device A can DMA-write to Device B's buffer without going through the IOMMU — bypassing all isolation.

Example topology without ACS:
```
IOMMU → PCIe Root Port → PCIe Switch (no ACS)
                              ├── NVMe drive
                              └── NVMe drive 2
```
Both NVMe drives are in the same IOMMU group. VFIO must pass both to the VM or neither.

With ACS: each PCIe device gets its own IOMMU group → fine-grained passthrough.

**Q2: How does the IOMMU framework handle the case where a device is probed but no IOMMU is present?**

The IOMMU framework has a graceful fallback path. During device registration:
1. `iommu_probe_device(dev)`: looks for an IOMMU hardware driver that claims this device
2. If no IOMMU driver registered (`bus_has_iommu() == false`): the DMA API falls back to the **direct DMA** path (`dma_direct_ops`)
3. Direct DMA: IOVA = physical address (or physical address - dma_pfn_offset)
4. Cache coherency: handled by ARM64 hardware (if coherent) or explicit cache ops (if not)

The device driver code stays identical — it calls `dma_map_single()` regardless. The DMA API dispatch table (`dev->dma_ops`) determines whether IOMMU or direct path is used. When an IOMMU driver loads later (or is enabled via boot param), `iommu_probe_device()` can be called again to enable IOMMU for existing devices.

---

## 7. Quick Reference

| IOMMU Concept | Description | ARM SMMU v3 Impl |
|---|---|---|
| iommu_domain | Translation table set | io_pgtable + ASID |
| iommu_group | Devices sharing domain | StreamIDs per group |
| iommu_ops | HW function table | arm_smmu_ops |
| iova_domain | IOVA allocator | Red-black tree |
| IOTLB | SMMU translation cache | Flushed via CMDQ |

| SMMU Fault | Cause | Kernel Action |
|---|---|---|
| F_TRANSLATION | No valid mapping | Bus error to device |
| F_ACCESS | Permission denied | Bus error to device |
| F_WALK_EABT | Page table walk failed | Kernel error report |
| F_ADDR_SIZE | Address out of range | Bus error to device |
