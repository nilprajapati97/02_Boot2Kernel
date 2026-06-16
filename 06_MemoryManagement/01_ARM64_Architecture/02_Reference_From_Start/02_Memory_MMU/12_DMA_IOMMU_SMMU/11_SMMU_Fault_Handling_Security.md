# SMMU Fault Handling and Security

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. SMMU Fault Classification

```
ARM SMMUv3 fault taxonomy:

Category 1: Translation Faults (DMA address not mapped)
  F_TRANSLATION: IOVA has no valid page table entry at any level
    Cause: device DMA to unmapped IOVA
    Fix: driver bug (forgot dma_map, or DMA after unmap)
    Action: device gets DECERR (slave error), SMMU logs event
  
  F_ADDR_SIZE: IOVA outside configured IPA/OA size
    Cause: device DMA to address wider than TCR.T0SZ allows
    Fix: driver set wrong DMA address width
  
  F_TRANSLATION_M: multi-level table entry malformed
    Cause: page table corruption or incomplete population

Category 2: Permission Faults
  F_PERMISSION: access type not allowed by AP/XN bits in PTE
    Cause: device writes to read-only IOVA, or device executes from no-exec region
    Fix: check iommu_map() flags: IOMMU_READ|IOMMU_WRITE|IOMMU_NOEXEC
  
  F_ACCESS: AF bit not set (if hardware AF not managed)
    Less common: SMMU usually manages AF automatically

Category 3: Configuration Faults (bad STE/CD)
  F_BAD_ATS_TREQ: ATS translation request malformed
  F_BAD_SUBSTREAMID: SubstreamID (PASID) > maximum in STE
    Cause: device sends PASID > smmu_domain.s1_cfg.s1cdmax
  F_STE_FETCH: error fetching STE from memory
    Cause: SMMU_STRTAB_BASE wrong, or memory not accessible to SMMU
  F_CD_FETCH: error fetching CD from memory
    Cause: STE.S1ContextPtr invalid

Category 4: Walk Faults
  F_WALK_EABT: external abort during page table walk
    Cause: SMMU walks page table, memory controller returns error
    Fix: hardware issue (ECC error, missing DRAM, TrustZone protection)
  
  F_WALK_SSERR: secure state error during walk (TrustZone)

Category 5: TLB / Sync Faults
  F_TLB_CONFLICT: TLB conflict during concurrent invalidation
    SMMU implementation-defined: usually auto-resolved by retry
  F_VMS_FETCH: VMID fetch error

SMMUv3 Event Queue entry format (64 bytes):
  Word 0[7:0]:   Type (fault type code)
  Word 0[31:8]:  StreamID
  Word 0[51:32]: SubstreamID (PASID)
  Word 0[55]:    Stall (1 = device stalled, needs response)
  Word 0[56]:    PnU (0 = prefetch, 1 = non-prefetch / real fault)
  Word 0[57]:    InD (instruction or data fault)
  Word 0[58]:    RnW (0 = read, 1 = write)
  Word 0[63]:    S2 (0 = S1 fault, 1 = S2 fault)
  Word 1[63:0]:  IOVA (faulting input address)
  Word 2[47:4]:  IPA (for S2 faults: intermediate physical address)
  ...
```

---

## 2. Event Queue Processing

```
EVTQ (Event Queue): SMMU → software fault ring buffer

Layout: identical to CMDQ (ring buffer in normal memory)
  SMMU_EVTQ_BASE: base physical address
  SMMU_EVTQ_PROD: producer pointer (written by SMMU hardware)
  SMMU_EVTQ_CONS: consumer pointer (written by software)
  SMMU_EVTQ_IRQ_CFG: configure IRQ on new event

IRQ handler: arm_smmu_evtq_handler()
  Called when SMMU EVTQ IRQ fires
  Reads events from EVTQ until CONS == PROD (queue empty)
  For each event:
    1. Decode event type and fields
    2. Find device: arm_smmu_find_master(smmu, StreamID)
    3. Report: iommu_report_device_fault(dev, &fault_data)
    
    If fault handler registered (SVM/PRI use case):
      driver's fault handler called
      Handler: may fault-in the page, then respond to SMMU
    
    If no handler (normal device fault):
      dev_err("SMMU event: type=%x StreamID=%x IOVA=%llx\n", ...)
      dev_err: printed to kernel log
      Return DECERR to device (device sees bus error)
      Optionally: arm_smmu_iommu_config_abort(dev): quarantine device

Linux EVTQ handling code:
  drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:
    arm_smmu_evtq_thread(): kernel thread that processes EVTQ
    arm_smmu_handle_evt(smmu, evt):
      switch (evt.type):
        case EVT_ID_TRANSLATION_FAULT:
          fault.type = IOMMU_FAULT_DMA_UNRECOV;
          fault.event.addr = evt.input_addr;
          iommu_report_device_fault(master->dev, &fault_event);
        case EVT_ID_PERMISSION_FAULT:
          similar handling
        ...

iommu_report_device_fault():
  Searches for registered per-device fault handler
  If found: calls handler (for SVM/PRI recovery)
  If not found: logs fault, no recovery action
  Returns: IOMMU_PAGE_RESP_HANDLED or IOMMU_PAGE_RESP_INVALID
```

---

## 3. SMMU Stall Model for SVM

```
SMMU Stall Model: device waits for kernel to handle fault

Normal TERMINATE model:
  Translation fault → SMMU returns error to device → device sees bus error
  Device: DMA fails, driver notified
  No recovery possible (permanent failure for this DMA op)

STALL model (needed for SVM/PRI):
  Translation fault → SMMU stalls device (holds DMA request)
  Device: DMA does NOT fail yet — device is paused
  Kernel: EVTQ event with Stall=1, fault->flags = IOMMU_FAULT_PAGE_REQUEST
  Handler:
    1. Identify missing page: fault->prm.addr = unmapped IOVA
    2. handle_mm_fault(current->mm, fault->prm.addr, FAULT_FLAG_WRITE)
       (same as CPU page fault handler!)
    3. Page allocated and mapped in process page tables
    4. SMMU Stage 1 CD[PASID].TTB0 = process page tables → new mapping visible
    5. ARM64: post CMD_PRI_RESP(success, StreamID, PASID, GRPID) to CMDQ
    6. CMD_SYNC: ensure SMMU processes PRI_RESP
  SMMU: resumes stalled device
  Device: DMA retries → now succeeds!
  
  This is exactly how GPU demand paging works:
    GPU code: access virtual address 0x100000 (not yet faulted in)
    GPU issues DMA: SMMU: translation fault (not mapped)
    SMMU stalls GPU
    Kernel: fault-in the page (alloc + map)
    SMMU: respond success → GPU DMA continues
    Programmer: never sees the fault (transparent to GPU code)

ARM64 SMMUv3 stall model requirements:
  SMMU_IDR0.STALL_MODEL = 0b00 (stall supported, not forced)
  OR 0b01 (stall forced): all faults become stalls regardless of STE
  STE.S1STALLD = 0: enable stall for this device
  CD.S = 0: enable stall for this CD
  
  Linux: arm_smmu_make_cdtable_ent():
    if (domain->type == IOMMU_DOMAIN_SVA):
      cd.s = 0  // enable stall
    else:
      cd.s = 1  // terminate (no stall, immediate error)
```

---

## 4. SMMU Security Integration with TrustZone

```
ARM TrustZone + SMMU security model:

Secure world (EL3/S-EL1): TF-A (Trusted Firmware-A) / OP-TEE
Non-secure world (EL0/EL1/EL2): Linux, VMs, applications

SMMU security configuration:
  SMMU can protect secure memory from non-secure DMA:
    STE.NS (non-secure): 0 = secure context, 1 = non-secure context
    Non-secure DMA to secure memory → SMMU ABORT (secure region protected)
  
  TZASC (TrustZone Address Space Controller):
    Separate IP from SMMU (but complementary)
    TZASC protects DRAM regions: mark region as "secure only"
    Non-secure DMA (even bypassing SMMU): TZASC blocks it
    SMMU + TZASC = two layers of DMA protection

  SMC (Secure Monitor Call) to EL3:
    Linux SMMU driver: some SMMU configuration locked by EL3
    arm-smmu-v3: on boot, SMC call to EL3 to finalize SMMU config
    EL3: may lock SMMU_GBPA.ABORT = 1 permanently (cannot be changed by Linux)
    This prevents: malicious Linux from enabling SMMU bypass

  pKVM (protected KVM) SMMU:
    Host Linux: configures SMMU Stage 1 (per-device)
    pKVM hypervisor: controls SMMU Stage 2 (per-VM)
    pKVM patches in SMMU driver:
      arm_smmu_alloc_domain(): if pKVM: delegate S2 to hypervisor
      iommu_attach_device(): hypervisor programs STE.S2TTB (Linux cannot override)
    Security guarantee: even if host Linux is compromised, VM isolation holds
    
  ARM CCA (Confidential Compute Architecture) / Realm:
    Realm VMs: memory encrypted, DMA cannot access encrypted RAM
    SMMU must be aware of Realm PA regions
    RMM (Realm Management Monitor) at EL3: controls which PA regions are Realm
    SMMU + TZASC: protect Realm memory from DMA
    dma_alloc_coherent() in Realm VMs: uses shared (unencrypted) memory region
    swiotlb used as DMA bounce buffer for Realm VMs
```

---

## 5. Debugging SMMU Faults

```
Debugging tools for SMMU translation faults:

1. Kernel log (dmesg):
   arm-smmu-v3 arm-smmu: event[0]=0x04000002 event[1]=0xdeadbeef
   arm-smmu: FAULT at IOVA 0xdeadbeef, StreamID 0x100, type F_TRANSLATION
   
   Decode: StreamID 0x100 = Bus 1, Dev 0, Fn 0 = look up /sys/bus/pci/devices/

2. /sys/kernel/debug/arm-smmu-v3/:
   arm_smmu.debugfs entries: (depends on kernel config)
   - evtq: event queue contents
   - cmdq: command queue state
   - strtab: stream table dump

3. iova_to_phys debugging:
   iommu_iova_to_phys(domain, iova):
     Walks SMMU page tables (in SW) → returns PA
     If fault: returns 0 (no mapping)
   Access via: /sys/kernel/debug/iommu/arm-smmu-v3/domains/

4. SMMU event counters (SMMUv3 PMU):
   ARM SMMUv3 has Performance Monitoring Unit (PMU):
     Event: SMMU_PMU_EVENT_TLB_MISS (IOTLB miss rate)
     Event: SMMU_PMU_EVENT_FAULT (fault count)
   perf stat -e arm_smmu/event=0x1/ -a sleep 1
   → Shows IOTLB miss rate (high = possible performance issue)

5. Dynamic debug:
   echo "file arm-smmu-v3.c +p" > /sys/kernel/debug/dynamic_debug/control
   → Enables pr_debug() in SMMU driver → verbose SMMU operations

6. smmu_domain_restore:
   If device gets quarantined (put in ABORT mode):
   echo 1 > /sys/bus/pci/devices/0000:01:00.0/reset
   echo vfio-pci > /sys/bus/pci/devices/0000:01:00.0/driver_override
   → After device reset: re-attach to driver, SMMU reconfigures

Common SMMU fault scenarios and solutions:
  
  dma_unmap without prior dma_map:
    F_TRANSLATION at bogus IOVA
    Fix: ensure every dma_unmap matches a dma_map
  
  DMA_FROM_DEVICE direction mismatch:
    Device tries to write to read-only mapping
    F_PERMISSION: write permission fault
    Fix: pass DMA_BIDIRECTIONAL or DMA_FROM_DEVICE (not DMA_TO_DEVICE)
  
  IOVA 0 (NULL IOVA bug):
    Some drivers bug: dma_addr_t initialized to 0, later used for DMA
    F_TRANSLATION at IOVA=0 (usually no valid mapping at 0)
    Fix: check dma_mapping_error(dev, dma_addr) after dma_map_*()
  
  IOMMU group not created:
    Device not found in stream table → global abort (if GBPA.ABORT=1)
    Fix: ensure device DTS has iommus = <&smmu N> property
```

---

## 6. Interview Questions & Answers

**Q1: A device driver calls dma_map_single() successfully, but the device gets a translation fault. What are the possible causes?**

Several scenarios can cause this:

1. **Direction mismatch**: Driver maps with `DMA_TO_DEVICE` (read-only mapping), but the device also writes (DMA_FROM_DEVICE). The SMMU PTE has write-protect bit set → F_PERMISSION fault. Fix: use `DMA_BIDIRECTIONAL`.

2. **IOMMU domain not configured**: If `iommu_attach_device()` was not called or failed silently, the STE for this StreamID might be in ABORT mode. `dma_map_single()` might succeed (allocates IOVA), but when device DMA arrives, SMMU aborts it.

3. **Wrong StreamID**: PCIe behind a bridge may have a different RID than expected. Stream Table Entry for the actual RID has no valid domain. Symptom: SMMU event shows unexpected StreamID.

4. **IOVA exhaustion and stale mapping**: Rare, but IOVA allocator exhaustion can cause `dma_map_single()` to return an already-unmapped IOVA (if IOVA recycling bug). Very rare.

5. **SMMU page table corruption**: Something overwrote SMMU page tables in memory. Hardware ECC error or kernel bug. The SMMU walk gets garbage PTE → fault.

6. **SMMU not configured (no STE entry)**: Device added after SMMU was initialized, but `iommu_probe_device()` wasn't called for it. STE slot is blank → default = ABORT.

**Q2: What is the difference between F_TRANSLATION and F_WALK_EABT?**

- **F_TRANSLATION**: The SMMU successfully walked the page tables but did not find a valid mapping. The page table entries were readable (no memory errors), but no valid PTE for the requested IOVA. Typically caused by: software bug (DMA to unmapped address), or use-after-unmap.

- **F_WALK_EABT** (External Abort during table walk): The SMMU encountered a memory error WHILE READING THE PAGE TABLE ITSELF. The SMMU tried to read a PTE from memory, but the memory controller returned an error (slave error / DECERR). Causes: (1) hardware ECC error in DRAM storing page tables, (2) page table memory protected by TZASC (inaccessible to SMMU), (3) page table at wrong physical address (software misconfigured CD.TTB0), (4) SMMU_STRTAB_BASE pointing to wrong memory.

F_WALK_EABT is more severe — it indicates hardware or severe software misconfiguration, not just a missing mapping. F_TRANSLATION is often a driver bug but can be recovered (for SVM with STALL model). F_WALK_EABT cannot be recovered — the SMMU literally cannot read the page tables.

---

## 7. Quick Reference

| Fault Type | Cause | Severity |
|---|---|---|
| F_TRANSLATION | No valid PTE | Medium (driver bug) |
| F_PERMISSION | Wrong access flags | Medium (driver bug) |
| F_ADDR_SIZE | Address out of range | Medium (config bug) |
| F_WALK_EABT | External abort reading PTE | High (hardware/severe bug) |
| F_BAD_SUBSTREAMID | PASID > max | Medium (SVM config) |
| F_STE_FETCH | Error reading STE | High (SMMU misconfiguration) |

| SMMU Model | Fault Response | SVM Use? |
|---|---|---|
| TERMINATE | Immediate error to device | No |
| STALL | Device paused, kernel handles | Yes (demand paging) |
| ABORT (STE.Config=ABORT) | Error regardless of mapping | Never (quarantine) |
