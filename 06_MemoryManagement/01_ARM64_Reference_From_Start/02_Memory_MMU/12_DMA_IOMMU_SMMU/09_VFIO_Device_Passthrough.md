# VFIO: Device Passthrough to VMs and Userspace

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. VFIO Concept

```
VFIO (Virtual Function I/O):
  Framework for safe userspace/VM access to PCIe and platform devices
  Goals:
    Security: device can only DMA to allowed memory regions
    Safety: device resources isolated from rest of system
    Performance: userspace/VM accesses device registers directly (no kernel round-trip)

Two use cases:

Use Case 1: Device passthrough to VM (KVM + QEMU + VFIO)
  Physical device → exclusively owned by one VM
  VM device driver: runs in guest kernel, talks to real hardware
  QEMU: forwards MMIO, IRQs, and DMA coordination
  IOMMU: protects host and other VMs from this VM's device
  
  Example: NVMe passed through to VM → VM's I/O bypasses hypervisor (near native speed)

Use Case 2: Userspace driver (DPDK, GPU, accelerator)
  Device accessed directly from userspace (no kernel driver)
  Example: Intel DPDK (Data Plane Dev Kit) for line-rate networking
  Example: AI accelerator accessed from Python (no kernel driver needed)
  VFIO-PCI: userspace maps BARs, receives IRQs via eventfd
  IOMMU: userspace driver cannot corrupt kernel memory via DMA
```

---

## 2. VFIO Architecture

```
VFIO three-level hierarchy:

              /dev/vfio/vfio (container)
                    │
                    ▼
         /dev/vfio/<group_id> (group)
                    │
                    ▼
             device_fd (device)

VFIO Container (/dev/vfio/vfio):
  Created once per userspace process
  Associates IOMMU domain with process
  DMA mappings set on container apply to all groups in container
  ioctl(container_fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU): enable IOMMU
  ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &dma_map):
    Create IOVA → PA mapping in SMMU for this device
    mmap() first: get PA for userspace buffer
    Then: map that PA into SMMU domain with given IOVA
  
VFIO Group (/dev/vfio/<group_id>):
  Each IOMMU group = one VFIO group
  Group number matches /sys/kernel/iommu_groups/<N>/
  ioctl(group_fd, VFIO_GROUP_SET_CONTAINER, &container_fd): attach to container
  ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, pci_addr): get device fd
  
  Rules:
    ALL devices in IOMMU group must be bound to VFIO before using any
    Cannot pass through one device from a group while leaving others in host
    → PCIe ACS required for fine-grained per-device passthrough

VFIO Device (device_fd):
  ioctl(device_fd, VFIO_DEVICE_GET_INFO): get device capabilities
  ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, &region): get BAR/config info
    region.index: VFIO_PCI_BAR0_REGION_INDEX, VFIO_PCI_CONFIG_REGION_INDEX
    region.offset: mmap offset for this region
    region.size: size in bytes
  mmap(NULL, bar_size, PROT_RW, MAP_SHARED, device_fd, bar_region_offset):
    Maps device BAR registers directly into userspace virtual address space
    Bypasses kernel: userspace writes to mapped VA → directly write device register
  ioctl(device_fd, VFIO_DEVICE_SET_IRQS, &irq_set):
    Configure IRQ → eventfd notification
    eventfd: userspace blocks on read(eventfd) waiting for device interrupt
```

---

## 3. KVM + VFIO + SMMU for VM Device Passthrough

```
Complete flow for passing NVMe to VM:

Host setup:
  1. pci_enable_sriov(pf_dev, 1): create 1 VF
  2. echo "vfio-pci" > /sys/bus/pci/.../driver_override
     echo "0000:01:00.1" > /sys/bus/pci/drivers/nvme/unbind
     echo "0000:01:00.1" > /sys/bus/pci/drivers/vfio-pci/bind
     → vfio-pci now owns VF
  
  IOMMU group:
     /sys/kernel/iommu_groups/5/devices/ = {0000:01:00.1}
     Group 5 = only this VF (ACS enabled → isolated group)
  
  SMMU configuration at bind time:
     vfio_iommu_type1_attach_group():
       iommu_domain_alloc(IOMMU_DOMAIN_UNMANAGED): create UNMANAGED domain
         (VFIO controls all mappings — not DMA API auto-managed)
       iommu_attach_device(domain, vf_dev):
         arm_smmu_attach_dev():
           Write STE[VF_StreamID]: S2TTB = VFIO's stage2 tables
           CMD_CFGI_STE + CMD_SYNC
         → SMMU now has VF connected to VFIO domain

QEMU VM memory setup:
  3. QEMU allocates guest RAM: mmap(NULL, guest_ram_size, PROT_RW, MAP_SHARED|MAP_ANON)
  4. ioctl(container_fd, VFIO_IOMMU_MAP_DMA, {
         iova: 0x0 (guest physical address 0)
         vaddr: guest_ram_vaddr
         size: guest_ram_size
         flags: READ|WRITE
     }):
     
     vfio_iommu_type1_map_dma():
       pin_user_pages(vaddr, size): pin guest RAM (no migration!)
       For each page: iommu_map(domain, iova, page_pa, PAGE_SIZE, RW)
         → arm_smmu_map_pages(): write SMMU PTEs: guest_PA → host_PA
       
     → SMMU now: device DMA to guest physical addr → SMMU translates → host physical page

KVM integration:
  5. KVM starts VM with vCPUs
  6. VM's NVMe driver programs VF's DMA registers with guest physical addresses (IOVAs)
  7. VF hardware issues PCIe DMA: addr = 0x100000 (guest PA)
  8. SMMU: STE[VF_StreamID] → Stage 1 disabled, Stage 2 translate:
          IPA 0x100000 → look up VFIO domain's page tables → host PA = mmap'd guest RAM
  9. DMA reaches correct host physical page (QEMU's guest RAM)
  10. VM isolation: VF cannot DMA outside VFIO_MAP_DMA'd range
      → Other VMs' memory: no mapping in this domain → translation fault → device gets error

ARM64-specific: SMMU Stage 2 for VFIO:
  VFIO domain → IOMMU_DOMAIN_UNMANAGED (VFIO manages all mappings)
  SMMU: uses Stage 2 for VFIO domain (maps guest PA = IPA → host PA)
  STE.Config = 0b110 (Stage 2 only, no Stage 1)
  STE.S2TTB = VFIO domain's stage 2 page table base
  VTCR = control register for guest physical address size
```

---

## 4. VFIO DMA Mapping Details

```c
/* VFIO DMA mapping structure */
struct vfio_iommu_type1_dma_map {
    __u32 argsz;
    __u32 flags;          // VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE
    __u64 vaddr;          // userspace virtual address (source)
    __u64 iova;           // device IOVA (destination — what device uses)
    __u64 size;           // mapping size
};

/* Unmap */
struct vfio_iommu_type1_dma_unmap {
    __u32 argsz;
    __u32 flags;
    __u64 iova;
    __u64 size;
};

VFIO_IOMMU_MAP_DMA implementation:
  vfio_iommu_type1_map_dma():
    1. Validate: IOVA within allowed range, size page-aligned
    2. pin_user_pages_fast(vaddr, npages, gup_flags, pages):
       Pin all pages (prevent migration/swap while device uses them)
       GUP (Get User Pages) returns struct page[] array
    3. For each page[i]:
       pa = page_to_phys(pages[i])
       iommu_map(domain, iova + i*PAGE_SIZE, pa, PAGE_SIZE, prot)
       → arm_smmu_map_pages(): write SMMU PTE
    4. Store mapping in interval tree (for later unmap/inquiry)
    5. Returns 0 on success

VFIO_IOMMU_UNMAP_DMA:
  vfio_iommu_type1_unmap_dma():
    1. Find mapping in interval tree by IOVA
    2. iommu_unmap(domain, iova, size)
    3. arm_smmu_iotlb_sync(): CMD_TLBI + CMD_SYNC (ensure SMMU TLB clean)
    4. unpin_user_pages(pages, npages): release page pins
    5. Remove from interval tree

Security enforcement:
  VFIO page pinning: pages cannot be swapped or migrated while mapped
  VFIO IOMMU mapping: device can only DMA to explicitly mapped pages
  No kernel bypass: VFIO ioctl validates all mappings through iommu_map()
```

---

## 5. VFIO Mediated Devices (mdev)

```
VFIO Mediated Device (vfio-mdev):
  Problem: pass through part of a device to multiple VMs simultaneously
  Example: GPU with 16GB VRAM → 4 VMs each get 4GB VRAM "slice"
  
  Physical device: GPU (full device, host-managed)
  mdev instances: virtual devices exposed to VMs (slices of GPU)
  
  How it works:
    Host GPU driver: creates mdev instances
    echo UUID > /sys/class/mdev_bus/0000:00:02.0/mdev_supported_types/.../create
    → Creates /sys/bus/mdev/devices/<UUID>
    VFIO: bind to vfio-mdev
    QEMU: pass /dev/vfio/mdev/<UUID> to VM
    
  IOMMU with mdev:
    mdev: does NOT have hardware isolation (shares physical PCIe function)
    VFIO mdev IOMMU: "emulated" — the physical driver mediates all DMA
    Physical driver: validates all DMA requests from VM before passing to hardware
    Less security than true SR-IOV VF: relies on software mediation
    
  ARM64 mdev examples:
    ARM Mali GPU: host driver creates per-context mdev instances
    Virtio: virtio-vhost-user uses mdev for efficient VM networking
    GPU time-slicing: one physical GPU, multiple VMs share via mdev

ARM64 VFIO security considerations:
  DMA attack: VM programs device to DMA to arbitrary host PA
    Mitigation: SMMU Stage 2 strictly enforced
    Only VFIO_MAP_DMA'd regions accessible
  
  Dirty page tracking (VFIO_IOMMU_GET_INFO.cap DIRTY_TRACKING):
    For live migration: track which guest RAM pages were DMA-written
    SMMU dirty bit (FEAT_HAFDBS): hardware logs dirty bits in PTEs
    ARM64: SMMU v3.2+ supports HAFDBS (Hardware Access/Dirty Flag management)
    Linux: arm_smmu_domain_enable_dirty_tracking()
```

---

## 6. Interview Questions & Answers

**Q1: What is the IOMMU type1 in VFIO and what does it provide?**

`VFIO_TYPE1_IOMMU` (also called `vfio_iommu_type1`) is the VFIO IOMMU backend that uses the Linux IOMMU framework (arm-smmu, intel-iommu, amd-vi, etc.) to provide DMA isolation.

When userspace calls `ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)`:
1. `vfio_iommu_type1_open()` creates an unmanaged IOMMU domain
2. Subsequent `VFIO_GROUP_SET_CONTAINER` calls attach IOMMU groups to this domain
3. `VFIO_IOMMU_MAP_DMA` ioctls create explicit IOVA → PA mappings

Type1 IOMMU guarantees:
- Device can only DMA to pages explicitly mapped via `VFIO_IOMMU_MAP_DMA`
- All mapped pages are pinned (cannot be swapped or migrated)
- IOMMU hardware enforces: unmapped IOVAs → translation fault → device error

The "type1" name comes from the IOMMU domain type: `IOMMU_DOMAIN_UNMANAGED` — userspace has full control over mappings rather than the DMA API.

For nested translation (VM with its own nested IOMMU): `VFIO_TYPE1_NESTING_IOMMU` enables Stage 1+2 nesting — guest configures S1, hypervisor sets S2. This is specifically useful on ARM64 where SMMUv3 nesting enables guest OS to manage its own IOMMU domain within the hypervisor's S2 protection.

**Q2: Why must ALL devices in an IOMMU group be passed through to the VM? Can't you just pass through one?**

You cannot pass through one device while leaving others in the same IOMMU group in the host — this would completely defeat IOMMU isolation:

If Device A (in host) and Device B (in VM) share an IOMMU group, they cannot be isolated from each other at the hardware level (e.g., PCIe without ACS). Device A (host-controlled) can DMA-write to Device B's descriptor rings, or Device B can read from Device A's memory. If Device B is in a potentially malicious VM, the VM could instruct Device B to DMA-read from Device A's memory → exfiltrate host kernel memory.

The IOMMU group semantics: "all devices in this group see the same IOMMU domain, and cannot be isolated from each other". Therefore, either all devices in the group are in the VM's domain (full passthrough), or none are.

Practical implication: on servers without ACS, a PCIe switch with 8 NVMe drives may be one large IOMMU group → you'd have to pass all 8 drives to one VM, or none. This is why enterprise servers specifically require ACS support in PCIe root ports and switches.

---

## 7. Quick Reference

| VFIO Level | Description | Key ioctls |
|---|---|---|
| Container (`/dev/vfio/vfio`) | IOMMU domain, DMA mappings | SET_IOMMU, MAP_DMA, UNMAP_DMA |
| Group (`/dev/vfio/N`) | IOMMU group (all devices isolated together) | SET_CONTAINER, GET_DEVICE_FD |
| Device (`device_fd`) | Individual PCIe function | GET_INFO, GET_REGION, SET_IRQS |

| SMMU + VFIO | Configuration |
|---|---|
| Domain type | IOMMU_DOMAIN_UNMANAGED |
| Stage | Stage 2 only (IPA → PA) for VM passthrough |
| STE Config | 0b110 (S2) |
| TLBI on unmap | CMD_TLBI_S2_IPA + CMD_SYNC |
| Dirty tracking | HAFDBS (SMMUv3.2+) |
