# DMA and IOMMU: Complete Interview Reference

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. DMA API Decision Tree

```
Q: Which DMA API should I use?

                        [Need DMA memory]
                              │
                    ┌─────────▼─────────┐
                    │ Needs CPU access   │
                    │ between DMA ops?   │
                    └─────────┬─────────┘
                    YES       │        NO (device-only buffer)
                   ┌──────────┘──────────┐
                   ▼                     ▼
         [Is it persistent?]    [DMA only, no CPU access]
              │                     → dma_alloc_coherent()
       YES    │    NO                  (rare: firmware buffers)
       ┌──────┘──┐
       ▼         ▼
  [Descriptor  [Transfer buffer]
   ring /        │
   control]       │
       │      ┌───▼────────────────┐
       │      │ Physically         │
       │      │ contiguous needed? │
       │      └───┬────────────────┘
       │      YES │    NO
       │      ┌───┘    └──┐
       │      ▼           ▼
       │  dma_map_single  dma_map_sg()
       │  (single phys)   (scatter-gather)
       │
       ▼
  dma_alloc_coherent()
  (descriptor rings)
  
  
Summary of DMA APIs:

dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL):
  → Allocates coherent buffer (CPU + device always in sync)
  → Returns: cpu_addr (kernel VA), dma_handle (device IOVA)
  → dma_free_coherent() to free
  → Use for: DMA rings, shared control structures

dma_map_single(dev, ptr, size, dir):
  → Maps single contiguous buffer for DMA
  → Returns: dma_addr_t (device IOVA or PA)
  → dma_unmap_single() when done
  → dma_mapping_error(dev, dma_addr): check for error!
  → Use for: single send/receive buffers

dma_map_sg(dev, sg, nents, dir):
  → Maps scatter-gather list (non-contiguous pages)
  → Returns: number of DMA segments (may coalesce with IOMMU)
  → dma_unmap_sg() when done
  → Use for: page-based I/O (network, storage, filesystems)

dma_pool_create() + dma_pool_alloc():
  → Small coherent allocations from a pool
  → Similar to kmem_cache but for DMA memory
  → Use for: frequently allocated small DMA descriptors

DMA direction:
  DMA_TO_DEVICE:   CPU writes, device reads (transmit/write)
  DMA_FROM_DEVICE: Device writes, CPU reads (receive/read)
  DMA_BIDIRECTIONAL: Both (avoid; use specific directions when possible)
  DMA_NONE: error/debugging only
```

---

## 2. ARM SMMU Complete Translation Flow

```
Full SMMU v3 translation flow (NVMe DMA on ARM64):

Device: NVMe at PCIe Bus=1, Dev=0, Fn=0
StreamID = 0x0100 (RID: Bus:Dev:Fn = 1:0:0)
DMA type: Stage 1 only (no hypervisor)
DMA read to IOVA = 0x00012000

Step 1: SMMU receives PCIe TLP
  [Read TLP] [Length=4KB] [Addr=0x00012000] [RID=0x0100]

Step 2: STE lookup
  strtab_base + (0x0100 × sizeof(STE))
  STE.Valid = 1
  STE.Config = 0b101 (Stage 1 only)
  STE.S1ContextPtr = 0xFFFFF800A0000 (pointer to CD array)
  STE.S1CDMax = 0 (one CD, SubstreamID ignored)
  → load CD[0]

Step 3: CD lookup
  CD[0].Valid = 1
  CD[0].ASID = 0x55
  CD[0].TTB0 = 0xFFFF8001A0000000 (device's Stage 1 page table base)
  CD[0].TCR = [T0SZ=25, TG0=00 (4KB), IRGN0=01, ORGN0=01, SH0=11]
  → ARM64 virtual address space: 0..2^39 (512GB, T0SZ=25, 64-25=39)

Step 4: Stage 1 page table walk
  IOVA = 0x00012000
  T0SZ=25 → 39-bit IA → 3-level table (L1/L2/L3)
  
  L1 table: CD[0].TTB0[PA]
    Index: IOVA[38:30] = 0 → L1[0]
    L1[0] = table descriptor → L2_table_PA
  
  L2 table: L2_table_PA
    Index: IOVA[29:21] = 0 → L2[0]
    L2[0] = table descriptor → L3_table_PA
  
  L3 table: L3_table_PA
    Index: IOVA[20:12] = 0x12 (18) → L3[18]
    L3[18] = page descriptor: [PA=0x200012000 | AF=1 | SH=11 | AP=01 | NS=1 | v=1]
  
  Result: PA = 0x200012000

Step 5: Attribute check
  AP=01: Read-only (RO), Read allowed ✓
  AF=1: access flag set ✓
  NS=1: non-secure world ✓
  → Access granted

Step 6: DMA proceeds
  NVMe DMA reads from PA 0x200012000
  IOTLB: cache entry {ASID=0x55, IOVA=0x12000} → PA=0x200012000
  Next DMA to same IOVA: IOTLB hit, no page walk needed

Step 7: Software invalidation (when driver unmaps)
  iommu_unmap(domain, 0x12000, 4096)
  → arm_smmu_unmap_pages(): clear L3[18] (PTE = 0)
  → dsb(ishst)
  → CMD_TLBI_NH_VA(ASID=0x55, VA=0x12000) to CMDQ
  → CMD_SYNC to CMDQ
  → Poll CMDQ cons_ptr until past CMD_SYNC
  → free_iova(iovad, 0x12000)
  → physical page freed to buddy allocator
```

---

## 3. Linux IOMMU Framework Summary

```
Complete call chain from device driver to SMMU hardware:

1. Probe time (device discovery):
   arm_smmu_probe_device(dev):
     Find SMMU for device (via ACPI IORT or DT iommus= property)
     Allocate arm_smmu_master for this device (stores StreamIDs)
     
   arm_smmu_device_group(dev):
     Determine IOMMU group (PCIe: check ACS path)
     Create/return iommu_group
   
   iommu_group_get_or_create_default_domain(group):
     If DMA mode: allocate IOMMU_DOMAIN_DMA domain
     arm_smmu_domain_alloc(): allocate page tables, ASID, IOVA allocator
     arm_smmu_attach_dev(): configure STE in stream table

2. Map time (dma_map_single):
   dma_map_single(dev, ptr, size, dir)
   → iommu_dma_map_page()
   → alloc_iova(iovad, size): get IOVA from allocator
   → iommu_map(domain, iova, pa, size, prot)
   → arm_smmu_map_pages()
   → io_pgtable_ops.map_pages()
   → arm_lpae_map()
   → write PTE to page table in memory
   → dsb(ishst): PTE visible to SMMU

3. Sync time (if non-coherent):
   arch_sync_dma_for_device(ptr, size, dir):
     DC CIVAC for each cache line (flush CPU dirty data to DRAM)
     DSB ISH

4. Unmap time (dma_unmap_single):
   → iommu_dma_unmap_page()
   → iommu_unmap(domain, iova, size)
   → arm_smmu_unmap_pages(): clear PTE
   → arm_smmu_iotlb_sync(): CMD_TLBI + CMD_SYNC
   → free_iova(iovad, iova)
   → arch_sync_dma_for_cpu() if DMA_FROM_DEVICE: DC IVAC + DSB
```

---

## 4. Top 20 Interview Questions

**Q1: What is IOMMU and why is it critical for system security?**
An IOMMU (I/O Memory Management Unit) performs address translation for device DMA transactions, analogous to the CPU's MMU for software. Without it, any DMA-capable device can read/write any physical memory address — a DMA attack. With IOMMU: devices can only access explicitly mapped physical pages. ARM's IOMMU = SMMU (System Memory Management Unit).

**Q2: Explain the ARM SMMUv3 Stream Table.**
The Stream Table is a data structure (flat array or 2-level tree) that maps each device's StreamID to a Stream Table Entry (STE). The STE contains the device's translation configuration: translation mode (abort/bypass/S1/S2/S1+S2), pointer to Context Descriptor (for Stage 1), and Stage 2 page table base. StreamID = PCIe Requester ID (RID = Bus:Dev:Fn) for PCIe devices.

**Q3: What is the difference between Stage 1 and Stage 2 SMMU translation?**
Stage 1: IOVA → IPA (or PA), configured per-device via Context Descriptor (CD). Used by device driver / guest OS. Stage 2: IPA → PA, configured by hypervisor via STE. Used for VM isolation. Combined S1+S2: nested translation for VMs that also have their own IOMMU domains (e.g., guest OS with VFIO inside a VM).

**Q4: How does the CMDQ ring buffer work?**
CMDQ is a ring buffer in normal (cacheable) RAM. Software writes commands (TLBI, CFGI, SYNC) to the producer pointer, then writes the new producer pointer to SMMU_CMDQ_PROD MMIO register. The SMMU hardware reads from the consumer pointer asynchronously, executes each command, and advances the consumer pointer. CMD_SYNC signals completion; software polls cons_ptr to confirm all prior commands are done.

**Q5: What is IOMMU group and why does it matter for VFIO?**
An IOMMU group is the set of devices that cannot be isolated from each other by IOMMU hardware (typically due to PCIe without ACS). VFIO requires passing ALL devices in an IOMMU group to a VM, because any one device in the group can DMA to any other device in the same group, bypassing IOMMU protection.

**Q6: Explain dma_alloc_coherent vs dma_map_single.**
`dma_alloc_coherent`: allocates a persistent buffer that both CPU and device can access without explicit sync. Usually non-cacheable (or coherent interconnect). For descriptor rings, control structures. `dma_map_single`: temporarily maps an existing buffer for a single DMA transaction. CPU uses normal cached access; explicit sync calls (or map/unmap) maintain coherency. For data transfer buffers.

**Q7: What cache instructions are needed for non-coherent DMA on ARM64?**
Before device reads (DMA_TO_DEVICE): `DC CIVAC` (Clean and Invalidate to PoC) for each cache line + `DSB ISH`. Flushes CPU dirty data to DRAM so device sees fresh data.  
Before CPU reads after device writes (DMA_FROM_DEVICE at unmap): `DC IVAC` (Invalidate) for each cache line + `DSB ISH`. Discards stale CPU cache so CPU re-reads device-written data from DRAM.

**Q8: How does PASID enable Shared Virtual Memory (SVM)?**
PASID (Process Address Space ID) is a 20-bit identifier that the device includes in PCIe transactions. The SMMU uses SubstreamID = PASID to index the CD array in the STE. Each PASID maps to a Context Descriptor containing the process's page table (TTB0 = mm->pgd). The device then shares the process's virtual address space: the device's DMA address = the process's virtual address. No IOVA allocation or pinning needed.

**Q9: What is ATS (Address Translation Service) and what is PRI?**
ATS: PCIe protocol where the device requests translations from the SMMU and caches them in its own IOTLB. The device then uses cached translations without SMMU lookup. When SMMU mappings change, SMMU sends ATS Invalidation messages to flush device IOTLB entries. PRI (Page Request Interface): complementary to ATS. When a device's ATS request fails (page not present), device sends a PCIe PRI Page Request. Kernel handles the page fault (demand paging) then sends PRI Response to resume the device.

**Q10: Why is CMD_SYNC required after every TLBI command?**
The SMMU CMDQ is asynchronous. After posting CMD_TLBI_NH_VA, the SMMU hardware may not have processed it yet. Without CMD_SYNC, the software could free a physical page immediately after posting TLBI, but the SMMU IOTLB still has the old IOVA→PA entry, and a device DMA could use the stale entry to access the freed page. CMD_SYNC guarantees all prior commands complete; software only continues (free page, reallocate IOVA) after CMD_SYNC completes.

**Q11: What is SMMU ABORT mode and when does Linux use it?**
ABORT mode (STE.Config=0b000): all DMA from this StreamID returns bus error immediately. Used for: (1) devices not yet claimed by any driver (default safe state), (2) quarantining devices after translation faults (misbehaving device), (3) security policy for unknown PCIe cards inserted into a server.

**Q12: What is SR-IOV and how does it interact with SMMU?**
SR-IOV creates multiple Virtual Functions (VFs) from one Physical Function (PF). Each VF has its own PCIe RID → unique StreamID → independent SMMU STE. For VM passthrough: each VF gets its own SMMU Stage 2 page tables (IPA→PA) mapping only that VM's memory. VF DMA cannot access other VMs' memory because the Stage 2 table doesn't map it.

**Q13: Explain DMA_FQ (Flush Queue) lazy IOMMU mode.**
DMA_FQ queues TLBI requests instead of processing them synchronously. `dma_unmap_single()` clears the PTE, adds {IOVA, page} to a flush queue, and returns immediately — no SMMU CMD_SYNC wait. A periodic worker drains the queue: issues bulk CMD_TLBI + CMD_SYNC + frees pages. Pages are not freed until after CMD_SYNC confirms IOTLB is clean. Benefit: eliminates per-operation CMD_SYNC latency (2-5µs) at high I/O rates.

**Q14: How does VFIO provide security for userspace drivers?**
VFIO uses the IOMMU to restrict device DMA to only memory ranges explicitly mapped via `VFIO_IOMMU_MAP_DMA` ioctls. Userspace can mmap device BARs directly for register access. All DMA pages are pinned (preventing page migration while mapped). The SMMU enforces: device can only DMA to explicitly allowed physical pages. Even if userspace driver is buggy, it cannot cause device to read/write kernel memory.

**Q15: What is the PCIe ECAM and how is it used on ARM64?**
ECAM maps all PCIe configuration spaces into a physical address range: base + (bus<<20)|(dev<<15)|(fn<<12). Linux reads MCFG ACPI table (or DT) for the ECAM base, ioremap()s it as Device nGnRE memory, then accesses PCIe config registers via readl/writel. ARM64 requires Device memory type (non-speculative, non-bufferable) for PCIe config space due to side-effectful config reads.

**Q16: What happens if a Linux driver forgets to call dma_unmap_single()?**
IOVA leak: the IOVA range is never returned to the IOVA allocator (`iova_domain`). Over time, the IOVA space exhausts → subsequent `dma_map_single()` calls fail. Physical page pin leak: if using SVM/PASID with `pin_user_pages()`, the page remains pinned — cannot be reclaimed or migrated. SMMU PTE remains: wastes SMMU IOTLB entries. Kernel: WARN or error when process exits without unmapping.

**Q17: Explain F_WALK_EABT SMMU fault.**
`F_WALK_EABT` (External Abort during Table Walk) occurs when the SMMU tries to read a page table entry but the memory controller returns a bus error. Causes: hardware ECC error in DRAM holding the page tables, page table memory protected by TZASC (inaccessible to SMMU), software bug setting CD.TTB0 to invalid physical address, or SMMU_STRTAB_BASE pointing to wrong memory. More severe than F_TRANSLATION — hardware/configuration issue, not driver bug.

**Q18: How does pKVM use the SMMU for protected VMs?**
pKVM (protected KVM) splits hypervisor duties: host Linux controls Stage 1 (per-device page tables), the pKVM hypervisor at EL2 controls Stage 2 (per-VM IPA→PA). The pKVM patches to arm-smmu-v3 delegate Stage 2 configuration to a hypervisor hypercall (HVC). Host Linux cannot modify Stage 2 page tables directly. Even if host Linux is compromised, VMs are protected because only the hypervisor can change what physical memory each VM's devices can access.

**Q19: What is the SMMU Stall Model and when is it needed?**
Stall Model: when a translation fault occurs, SMMU pauses the device's DMA request (instead of returning error). The kernel handles the page fault (like CPU demand paging), then sends CMD_PRI_RESP(success) to resume the stalled device. Required for SVM (Shared Virtual Memory) where devices use process virtual addresses: pages may not be mapped initially and must be faulted in on demand. Normal device drivers don't need stall model (errors are detected and retried at driver level).

**Q20: Compare ARM SMMU v3 with Intel VT-d. Key differences?**
|Feature|ARM SMMUv3|Intel VT-d|
|---|---|---|
|Page table format|AArch64 (same as CPU)|x86-64 (separate format)|
|Device ID|StreamID (20-bit)|Source-ID (16-bit, BDF)|
|Command interface|CMDQ ring buffer|MMIO invalidation commands|
|PASID/SVM|SubstreamID + CD array|PASID-based IOTLB|
|Nested translation|S1+S2 (hardware)|First+second level (hardware)|
|Page Request Interface|PRI via EVTQ|Page Request Interface (PRI)|
|Fault reporting|EVTQ ring buffer|Fault registers + IRQ|
|ATS support|Yes (SMMUv3)|Yes (VT-d)|

---

## 5. Quick Reference: All DMA/IOMMU Concepts

| Concept | Definition | ARM64 Implementation |
|---|---|---|
| IOMMU | Device address translation HW | ARM SMMU |
| SMMU | ARM's name for IOMMU | SMMUv1/v2/v3 |
| StreamID | Device identifier for SMMU | PCIe RID for PCIe devices |
| STE | Per-device SMMU config | 64 bytes in Stream Table |
| CD | Per-context/process config | 64 bytes, pointed by STE |
| IOVA | Device's virtual address | Allocated by iova_domain |
| ASID | Address space ID for IOTLB | 16-bit, in CD |
| VMID | VM ID for Stage 2 IOTLB | 16-bit, in STE |
| PASID | Process ID for SVM | 20-bit = SubstreamID |
| CMDQ | Command ring buffer | 16-byte commands in RAM |
| EVTQ | Event ring buffer | 64-byte events from SMMU |
| ATS | Device IOTLB | STE.EATS, PCIe ATR/ATI |
| PRI | Device page request | EVTQ + CMD_PRI_RESP |
| VFIO | Userspace device access | IOMMU_DOMAIN_UNMANAGED |
| DMA_FQ | Lazy TLBI batch | Periodic CMD_TLBI + CMD_SYNC |
| Coherent DMA | No sync needed | dma_alloc_coherent() |
| Streaming DMA | Sync required | dma_map_single/sg() |

| ARM64 Cache Op | When | DSB Required? |
|---|---|---|
| DC CIVAC | Before device reads (DMA_TO_DEVICE) | Yes (DSB ISH) |
| DC IVAC | Before CPU reads after device write | Yes (DSB ISH) |
| DC CVAC | Writeback without evict | Yes (DSB ISH) |
| None | Coherent interconnect | No |
