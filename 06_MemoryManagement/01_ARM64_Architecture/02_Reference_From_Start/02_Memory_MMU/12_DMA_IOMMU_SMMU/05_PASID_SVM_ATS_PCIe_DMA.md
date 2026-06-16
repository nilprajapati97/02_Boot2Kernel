# PASID, Shared Virtual Memory, and PCIe ATS

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. PASID Concept

```
PASID (Process Address Space ID): extend device DMA to use process virtual addresses

Traditional DMA:
  Device uses kernel-allocated IOVAs (private kernel address space)
  Driver: alloc IOVA → map PA → program device with IOVA
  Device sees: custom IOVA space (not same as process virtual addresses)
  
  Problem: GPU, FPGA, accelerators want to share address space with the CPU process
    Process allocates virtual memory: ptr = malloc(100MB)
    Wants to pass ptr (virtual address!) directly to GPU
    GPU DMA should access same physical pages as the process
    Without SVM: must pin pages, create IOVA mappings, pass IOVA to GPU → complex!
  
PASID solution (SVM: Shared Virtual Memory):
  GPU (device) shares the process's page tables directly via SMMU
  Process's virtual address ptr = GPU's DMA address
  Process's page fault handler handles GPU page faults too!
  
  Technical basis:
    PASID: 20-bit identifier (from PCIe TLP prefix / ATS)
    Maps 1:1 with CPU process address space (mm_struct / ASID)
    SMMU SubstreamID = PASID
    SMMU CD indexed by SubstreamID: CD[PASID] = process's page table (TTB0)
    
  Process creates virtual memory: mmap(GPU_buffer, 1GB, ...)
  GPU issues DMA: includes PASID in PCIe transaction
  SMMU: looks up CD[PASID] → TTB0 = process's TTBR0 value
         walks process page tables: VA → PA
  GPU accesses exactly same memory as CPU process
  
  On page fault (VA not mapped yet):
    SMMU generates PRI (Page Request Interface) fault event
    Kernel's io-pgfault handler: calls process's do_page_fault()
    Page allocated, mapped in process page tables
    SMMU S1 now finds the mapping
    GPU DMA succeeds
```

---

## 2. PASID in ARM SMMUv3

```
SMMUv3 PASID support:

1. STE configuration for SVA device:
   STE.S1FMT = 0b01 (linear CD array) or 0b10 (pointer to CD array)
   STE.S1ContextPtr → CD array[0..2^PASID_bits-1]
   
   CD array: Linux allocates mm->pasid entries
   CD[0]: kernel identity (for DMA without PASID)
   CD[PASID]: process's page table (TTB0 = mm->pgd physical address)
              ASID = mm->context.id & 0xFFFF
              TCR = process's translation control

2. ARM64 PASID flow:
   a. sva_bind(dev, mm): bind process to device
      → alloc_pasid(mm): assign PASID to mm_struct
      → smmu_domain_sva_bind():
          Write CD[PASID]: TTB0 = __pa(mm->pgd), ASID = mm->context.id
          Post CMD_CFGI_CD(sid, pasid) to flush CD cache
      → return PASID to userspace (user tells GPU: "use PASID = N")
   
   b. GPU issues DMA with PASID in TLP:
      PCIe TLP: [PASID_TLP_PREFIX][PASID=N][DMA_address]
      SMMU receives: (StreamID, SubstreamID=N, address)
      SMMU S1 walk using CD[N]
   
   c. PRI fault (page not mapped):
      SMMU EVTQ: F_TRANSLATION event with SubstreamID=N, IOVA=VA
      Kernel: io_pgfault_handler()
              get mm from PASID, do_page_fault(mm, VA, read/write)
              After fault handled:
                Post CMD_PRI_RESP(success) to CMDQ
              SMMU retries: now finds valid page table entry

3. PASID limits on SMMUv3:
   Maximum PASID bits: 20 (up to 1M PASIDs per device)
   Linux default: PASID_BITS = 20
   mm_struct.pasid: assigned from xa_alloc (XArray, 1..max_pasid)

ARM64 SMMU PASID MMU notifier:
  When process page table changes (fork, munmap, mprotect):
    CPU: updates own page tables
    SMMU: must also see the change (process shares page tables with SMMU)
    
  mmu_notifier mechanism:
    mm_struct has list of mmu_notifiers
    When page table changes: call all notifiers
    sva_notifier: invalidates SMMU IOTLB for affected VA range
      Post CMD_TLBI_NH_VA(asid=mm_asid, iova=start..end) to CMDQ
```

---

## 3. PCIe ATS: Address Translation Service

```
PCIe ATS (Address Translation Service): device-side IOTLB

Traditional SMMU flow:
  Every PCIe DMA read: PCIe request → SMMU TLB lookup → (on miss) page walk → memory
  Problem: SMMU IOTLB misses add 100s of nanoseconds latency per miss

ATS solution: device has its own IOTLB
  1. Device sends ATS Translation Request (ATR): "translate IOVA X for me"
  2. SMMU receives ATR: does full S1 translation → returns PA + attributes
  3. Device stores: IOVA X → PA + perms in its own IOTLB
  4. Subsequent DMA reads: device uses cached translation → skips SMMU
  5. DMA goes directly: device → PCIe → memory controller (SMMU bypassed!)

ATS invalidation protocol (ATS Invalid Messages):
  When SMMU mapping changes (unmap, mprotect):
    SMMU/kernel: send ATS Invalidation Request to device via PCIe
    Device: flushes matching entries from its IOTLB
    Device: sends ATS Invalidation Completion
    Kernel: gets completion → can now free physical page or change mapping

ARM64 SMMUv3 ATS support:
  SMMU_IDR1.ATS: indicates ATS support
  STE.EATS[1:0]: enable ATS for this stream
    00: ATS disabled
    01: ATS enabled (SMMU honors ATR and sends Invalid messages)
  
  SMMUv3 ATS invalidation flow:
    kernel: iommu_unmap(domain, iova, size)
    → arm_smmu_tlb_inv_range_nosync(iova, size, ...)
    → arm_smmu_atc_inv_domain():
        For each PCIe device with ATS:
          Send: ATSInvalidationReq(iova, size) via SMMU CMDQ:
            CMD_ATC_INV: (sid, ssid, iova, size)
        Wait for responses: CMD_SYNC
    → Only after device IOTLB is clean: free physical page
  
  PRI (Page Request Interface): companion to ATS for SVM
    Device with ATS + PRI: when ATS translation fails (page not present):
      Device sends PRI Page Request message
      SMMU: event on EVTQ as F_PRI
      Kernel: allocate page, update page tables
      Kernel: send PRI Response via SMMU CMDQ: CMD_PRI_RESP(success)
      Device retries ATS request: now gets valid translation

Linux ATS driver support:
  iommu_dev_enable_feature(dev, IOMMU_DEV_FEAT_ATS):
    Enable ATS for device (program STE.EATS)
  iommu_dev_enable_feature(dev, IOMMU_DEV_FEAT_SVA):
    Enable SVA (SVM with PASID) for device
  pci_enable_ats(pdev, ps): enable ATS on PCIe device
  pci_ats_page_aligned(pdev): check ATS page alignment requirement
```

---

## 4. DMA Coherency Domains

```
ARM64 cache coherency domains for DMA:

Inner Shareable domain (IS):
  Scope: all CPUs in one cluster sharing L2/L3 cache
  Example: 4 Cortex-A76 cores sharing 8MB L3 cache
  MAIR.SH = 0b11: marks memory as Inner Shareable
  Cache operations with ISH barrier:
    DC CIVAC + DSB ISH: visible to all IS-domain observers after barrier

Outer Shareable domain (OS):
  Scope: multiple clusters + some DMA masters (if on coherent interconnect)
  Example: 2 clusters + GPU + PCIe all on CMN-700
  DMA masters in OS domain: see coherent cache state
  MAIR.SH = 0b10: marks memory as Outer Shareable

Non-shareable:
  MAIR.SH = 0b00: private (only this CPU/device)
  Normal use: stack pages, truly private data

Full system:
  MAIR.SH = 0b11 (IS = all CPUs): standard for shared kernel memory
  DSB SY: waits for ALL system observers (full system barrier)

DMA coherency in practice on ARM64:
  ARM CMN-600/700/CMN-Mesh: Coherent Mesh Network
    Connects: CPUs, PCIe root complex, eMMC, USB, Ethernet, GPU
    All endpoints participate in coherency protocol (ACE-Lite or CHI)
    DMA read: see most recent CPU write (in cache or DRAM)
    DMA write: CPU read sees device's data (invalidates CPU caches)
    → dma_map_single() = just return physical address, no cache ops
  
  On-chip devices NOT on CMN:
    Low-power microcontrollers, sensor hub, audio DSP
    Often NOT coherent (separate power domain)
    Need software cache maintenance
    → dma_map_single() = DC CIVAC/IVAC + DSB before/after
```

---

## 5. Interview Questions & Answers

**Q1: What is Shared Virtual Memory (SVM) and why is it needed for modern GPU workloads?**

SVM allows a device (GPU, FPGA, AI accelerator) to directly use the **same virtual addresses** as the CPU process — no separate IOVA allocation, no pin-and-map step.

Traditional GPU workflow WITHOUT SVM:
```
1. CPU: pin_user_pages(user_buffer)     // prevent page migration
2. CPU: dma_map_sg(gpu_dev, pages, ...) // create IOVA → PA mapping
3. CPU: send IOVA to GPU               // tell GPU which address to use
4. GPU: DMA using IOVA                  // GPU accesses memory
5. CPU: dma_unmap_sg(...)               // release IOVA
6. CPU: unpin_user_pages(...)           // allow page migration again
```
Problem: pinning prevents page migration (hurts NUMA balancing), requires explicit synchronization, cannot fault-in pages on demand.

WITH SVM:
```
1. CPU: sva_bind(gpu_dev, current->mm, &pasid)  // bind process
2. CPU: send (user_buffer_VA, pasid) to GPU      // simple!
3. GPU: DMA with PASID=pasid, address=user_buffer_VA
        → SMMU walks process page tables → same PA as CPU
4. If page not yet faulted in: SMMU sends PRI → kernel allocates → GPU continues
5. sva_unbind() when done
```

Benefits: zero-copy (shared pages), on-demand paging (no pre-pinning), transparent huge pages automatically used, NUMA migration still works.

**Q2: Explain ATS invalidation. Why is it complex and what can go wrong?**

ATS (Address Translation Service) lets devices cache IOVA → PA translations in their own IOTLB. This improves performance (bypasses SMMU for cached entries) but creates a coherency problem: when the CPU unmaps a page, the device's IOTLB may still have the old translation.

**The invalidation protocol**:
1. Software calls `iommu_unmap(domain, iova, size)` to remove mapping
2. SMMU's IOTLB is invalidated immediately (via CMDQ)
3. But device's IOTLB may still have stale entry!
4. SMMU sends PCIe ATS Invalidation Request message to device
5. Device MUST: (a) invalidate matching IOTLB entries and (b) complete all DMA using those translations
6. Device sends PCIe ATS Invalidation Completion
7. Only after receiving completion: kernel may free the physical page

**What can go wrong**:
- **Timeout**: device takes too long to respond → kernel times out → may free page while device still using it → memory corruption
- **Device crash**: device hangs, never sends completion → kernel stuck
- **Race**: device issues new ATS request for same IOVA after invalidation starts → gets new mapping or fault (protocol handles this with `InvalidationCompletion` count)
- **No PRI**: if device has ATS but no PRI: on SVM page fault, device simply gets bus error (translation not found) → GPU gets fault signal → application error. PRI adds fault-in capability.

Linux ATS timeout: controlled by `iommu_atc_inv_timeout`. If device doesn't respond: error reported, device may be reset or disabled.

---

## 6. Quick Reference

| Technology | Purpose | SMMU Feature |
|---|---|---|
| PASID | Per-process DMA namespacing | SubstreamID → CD array |
| SVM | Share process VA with device | CD[PASID] = mm->pgd |
| ATS | Device-local IOTLB cache | STE.EATS, ATR/ATI messages |
| PRI | Demand paging for devices | CMD_PRI_RESP, EVTQ fault events |

| PASID Use Case | SMMU Config |
|---|---|
| Kernel DMA (no PASID) | CD[0] = kernel page tables |
| Process GPU access | CD[PASID] = process mm->pgd |
| VM device passthrough | Stage 2 (IPA → PA via VTTBR) |
| Nested (VM + PASID) | Stage 1 (CD[PASID]) + Stage 2 |
