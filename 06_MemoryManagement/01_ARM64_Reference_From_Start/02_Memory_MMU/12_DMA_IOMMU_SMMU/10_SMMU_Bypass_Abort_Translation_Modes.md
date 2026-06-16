# SMMU Bypass, Abort, and Translation Modes

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. SMMU Translation Modes Overview

```
SMMU STE.Config field controls translation for each StreamID:

STE.Config[3:1]:
  b000 = ABORT:   All DMA → abort (bus error returned to device)
  b100 = BYPASS:  No translation (device sees physical addresses directly)
  b101 = S1 only: Stage 1 only (IOVA → PA via kernel page tables)
  b110 = S2 only: Stage 2 only (IPA → PA via hypervisor page tables)
  b111 = S1+S2:   Stage 1 + Stage 2 (IOVA → IPA → PA)

Valid bit (STE.Config[0]):
  0 = Invalid STE entry (device not recognized → abort)
  1 = Valid STE entry (use Config above)

Default behavior on SMMU probe:
  Before any driver claims device: STE invalid → ABORT
  After iommu_attach_device(): STE configured per domain type
  
  iommu.passthrough=0 (default in most distros):
    New devices: DMA-managed domain → S1 translation
    Safer but slight overhead
  
  iommu.passthrough=1:
    New devices: bypass (identity) domain → BYPASS mode
    Performance benefit (no IOMMU overhead)
    Security cost (no DMA isolation)
    Use in: trusted environments, benchmarking, legacy hardware

ARM64 SMMUv3 STE format (first word):
  Bit 0:      Valid
  Bits [4:1]: Config
  Bit 5:      S1FMT (stage 1 format: linear or pointer)
  Bits [51:6]: S1ContextPtr (pointer to CD array)
  Bits [63:52]: Reserved
```

---

## 2. ABORT Mode: Security Hardening

```
ABORT mode: all DMA from this StreamID → immediate abort
  SMMU returns: DECERR (slave error) to PCIe device via TLP completion
  PCIe device: receives unsupported request (UR) completion → bus error

When ABORT is used:
  1. Initial state: before device driver loads
     Linux SMMUv3: uninitialized STEs are invalid → treated as ABORT
     Prevents: devices from DMA before driver configures IOMMU
  
  2. Device quarantine (after fault):
     arm_smmu_iommu_config_abort():
       Write STE: Config = ABORT, Valid = 1
       CMD_CFGI_STE + CMD_SYNC
     Device now: all DMA → abort
     Reason: after translation fault, device may be misbehaving
     Admin must: reset device to recover
  
  3. Security policy: untrusted/unknown device
     Server: IOMMU group for PCI slot = ABORT by default
     If no authorized driver claims device: stays in ABORT
     Prevents: inserted rogue PCIe card from DMA-reading memory

  4. iommu.strict_DMA_timeout:
     If DMA mapping times out (map can't complete): device put in ABORT
     Prevents: hung device from bypassing isolation

Fault generation with ABORT:
  Device issues DMA: STE lookup → Config=ABORT
  SMMU: generates F_TRANSACTION (fault event) AND returns error to device
  Event Queue: records (StreamID, IOVA, fault_type=abort)
  Linux: arm_smmu_handle_evt() → dev_err("SMMU: DMA abort for StreamID %d")
  
  NOTE: unlike translation fault, ABORT events are for all accesses regardless
        of whether the address would have been valid anyway
```

---

## 3. BYPASS Mode: Performance vs Security Tradeoff

```
BYPASS mode: no translation, device address = physical address
  STE.Config = 0b100
  DMA address from device: passed directly to memory bus
  No SMMU page table walk, no IOTLB lookup
  
  Performance benefit:
    Zero SMMU overhead (no IOTLB miss cost, no page walk cost)
    No IOVA allocation overhead
    DMA descriptor: contains physical address directly
    
  Security risk:
    Device can DMA to ANY physical address
    Malicious or compromised device: can read kernel memory, steal keys
    Bug in driver: can corrupt arbitrary kernel or user memory
    
  When BYPASS is acceptable:
    1. Fully trusted device (internal SoC DMA controller, not on PCIe)
    2. Debugging/benchmarking: disable SMMU to measure overhead
    3. Legacy driver: not yet updated to use DMA API properly
    4. Embedded system: physically closed, no untrusted devices
    5. RISC-V/Arm embedded SoCs: limited threat model (no PCIe)

Linux BYPASS domain:
  iommu_domain type: IOMMU_DOMAIN_IDENTITY
  arm_smmu_domain_alloc(IOMMU_DOMAIN_IDENTITY):
    → set arm_smmu_domain.stage = ARM_SMMU_DOMAIN_BYPASS
  iommu_attach_device(identity_domain, dev):
    → arm_smmu_write_strtab_ent(Config=BYPASS)
    → CMD_CFGI_STE + CMD_SYNC
  
  DMA API with identity domain:
    dma_map_single(): returns physical address unchanged (IOVA = PA)
    dma_unmap_single(): no-op
    No cache maintenance (assumed coherent or non-cacheable as normal)
  
  Linux module parameter:
    iommu.passthrough=1 → all new devices get IDENTITY domain (BYPASS)
    CONFIG_IOMMU_DEFAULT_PASSTHROUGH: compile-time default
    CONFIG_IOMMU_DEFAULT_DMA_STRICT: strict DMA domain (most secure, default)

BYPASS vs IDENTITY domain distinction in Linux:
  BYPASS: SMMU hardware does no translation (STE.Config=BYPASS)
  IDENTITY: Linux DMA API returns PA directly (may or may not configure BYPASS in hw)
            On coherent systems without SMMU: always identity
            On SMMU systems: IDENTITY domain → SMMU configured as BYPASS
```

---

## 4. DMA Lazy Unmap (IOMMU_DOMAIN_DMA_FQ)

```
DMA_FQ (Flush Queue) domain: performance optimization for mapping/unmapping

Problem with strict mode:
  dma_unmap_single():
    1. Remove SMMU page table entry (PTE)
    2. CMD_TLBI_NH_VA + CMD_SYNC (wait ~1-2µs for SMMU TLB flush)
    3. Free IOVA
    4. Free physical page
  
  The CMD_SYNC wait: synchronous, blocks CPU
  For high-rate I/O (NVMe at 1M IOPS): 1M × 2µs = 2s/s wasted just on IOTLB flushes!

DMA_FQ (Flush Queue) optimization:
  iommu_domain type: IOMMU_DOMAIN_DMA_FQ
  
  dma_unmap_single() with DMA_FQ:
    1. Remove SMMU PTE
    2. Add {IOVA, freed page} to flush queue (in-memory ring buffer)
    3. Return immediately! No CMD_SYNC wait!
  
  Periodic flush worker (iommu_dma_flush_iotlb_all()):
    Every N ms or when queue full:
      CMD_TLBI_NH_ASID (all pending IOVAs in one go) + CMD_SYNC
      Wait for CMD_SYNC
      Free all queued IOVAs + pages
  
  Safety: freed physical pages are NOT returned to buddy allocator
          until after CMD_SYNC confirms IOTLB is clean
          → Even if IOTLB has stale entry: page still allocated (not reused)
          → Device DMA hits stale mapping → reads/writes to still-allocated page
          → Harmless: page contents may be garbage, but no security violation
          → IOVA is not reused until after flush → no "IOVA reuse attack"
  
  ARM64 SMMUv3 Linux DMA_FQ:
    arm_smmu_flush_iotlb_all(domain):
      Uses CMD_TLBI_NH_ASID for domain's ASID
    
    CONFIG_IOMMU_DEFAULT_DMA_LAZY → select DMA_FQ domain
    iommu.dma_domain=lazy: runtime selection

  Performance gain with DMA_FQ:
    NVMe IOPS: 1M/s → before: 1M × 2µs = 100% CPU on flush
                       after:  1 flush per 1ms → 1000 × 2µs = 0.2% CPU
    Real-world: 20-30% I/O throughput improvement for small transfers
```

---

## 5. SMMU Security Configuration

```
ARM SMMU security hardening in Linux:

1. CONFIG_IOMMU_DEFAULT_DMA_STRICT: default for new devices = strict DMA domain
   Ensures: all device DMA requires explicit mapping
   Contrast: passthrough mode (bypass) = less secure, faster

2. SMMU hardware security registers (SMMU_IDR0.STALL_MODEL):
   STALL model: on translation fault, SMMU stalls device (retries after page handled)
   TERMINATE model: on translation fault, return error immediately (no stall)
   Linux default: TERMINATE (security > compatibility)
   STALL needed for: SVM/PRI (demand paging for devices)

3. SMMU_GBPA (Global Bypass/Abort): global register
   Before SMMU is programmed: what happens to ALL DMA?
   GBPA.ABORT = 1: all DMA aborted (safest, used at SMMU init)
   GBPA.ABORT = 0: all DMA bypassed (dangerous — pre-driver DMA unrestricted)
   Linux: set GBPA.ABORT = 1 at SMMU init, clear per-device after STE configured

4. Per-STE security:
   SMMU_PROP (propagate attributes): controls attribute propagation to AXI
   RES0 check: reserved bits in STE checked → fault if set
   
5. Transaction fault vs configuration fault:
   Translation fault: address not mapped → recoverable (page fault)
   Configuration fault: bad STE/CD format → SMMU abort (driver bug)
   Event Queue: records all faults with full context (StreamID, IOVA, type)

6. Hypervisor SMMU protection (pKVM):
   pKVM (protected KVM): hypervisor protects its own page tables from Linux host
   ARM SMMU: hypervisor programs Stage 2 directly (bypasses host kernel)
   Host kernel: can configure Stage 1 (per-device) but NOT Stage 2 (per-VM)
   pKVM patches: arm_smmu_v3_set_attribute(HYPMODE_STAGE2_ONLY)
   → VMs cannot escape via SMMU even if host kernel is compromised
```

---

## 6. Interview Questions & Answers

**Q1: What is the security risk of iommu.passthrough=1 and when is it safe to use?**

`iommu.passthrough=1` configures all new devices to use an identity (BYPASS) IOMMU domain. In BYPASS mode, the SMMU performs no address translation — the DMA address from the device goes directly to the memory bus as a physical address.

**Security risk**: A compromised or malicious device (or a driver bug) can DMA read from any physical address — including kernel code/data, other processes' memory, cryptographic keys in memory, or hypervisor memory. This is the classic DMA attack vector. Real-world example: FireWire DMA attacks on pre-IOMMU systems — plug in a device, DMA-read RAM, extract passwords. With PCIe, inserting a malicious PCIe card into a server with `passthrough=1` gives complete memory access.

**When it's safe**:
1. **Embedded/IoT systems** with fixed, trusted hardware (no external PCIe expansion)
2. **Performance benchmarking** to establish a baseline without IOMMU overhead
3. **Legacy hardware** where IOMMU mapping causes incorrect behavior (device requires physical addresses)
4. **Physically secured servers** where no untrusted hardware can be inserted (datacenter with physical access controls)
5. **Development/debugging** to isolate whether an issue is IOMMU-related

In production ARM64 server deployments (AWS, GCP, Azure): `iommu.passthrough=0` (strict mode) is always used to protect against DMA attacks from compromised network cards, NVMe drives, or tenant-inserted hardware.

**Q2: What happens to an in-flight DMA operation when dma_unmap_single() is called with DMA_FQ? Is there a race condition?**

With DMA_FQ (Flush Queue, "lazy IOMMU"), `dma_unmap_single()` queues the invalidation instead of doing it immediately:

1. SMMU PTE is cleared (page table entry removed)
2. The IOVA and freed page are added to the flush queue
3. The physical page is NOT freed yet — it stays allocated

The stale IOTLB entry may still be present. If a device is misbehaving (still DMA'ing after the driver called `dma_unmap_single()`):
- Device uses stale IOTLB entry → hits old physical page (still allocated, not reused)
- Device reads/writes garbage from/to that page → device gets confused, may error
- But: this is the device's fault for using a mapping after it was unmapped

Safety guarantee (no security violation): the physical page cannot be reused for any purpose until after the flush queue is drained and CMD_SYNC completes. The IOVA cannot be reallocated until after flush. So there is no "confused deputy" scenario where the device accidentally accesses another driver's data.

However, this does mean: a bug in a device driver that calls `dma_unmap_single()` while device is still DMA'ing is **less immediately catastrophic** with DMA_FQ (DMA continues to the old page, not corrupting something else), but the device state is likely corrupted — this is a driver bug, not a DMA_FQ issue.

---

## 7. Quick Reference

| STE Config | Translation | Security | Performance | Use Case |
|---|---|---|---|---|
| ABORT (0b000) | None (error) | Maximum | N/A | Quarantine, init state |
| BYPASS (0b100) | None (PA=DMA) | Minimum | Maximum | Trusted embedded, benchmarks |
| S1 only (0b101) | IOVA → PA | High | Good | Normal device DMA |
| S2 only (0b110) | IPA → PA | High (VM) | Good | VM device passthrough |
| S1+S2 (0b111) | IOVA → IPA → PA | Maximum | Moderate | Nested VM with device |

| DMA Domain Type | SMMU Mode | Flush Mode |
|---|---|---|
| IOMMU_DOMAIN_BLOCKED | ABORT | N/A |
| IOMMU_DOMAIN_IDENTITY | BYPASS | N/A |
| IOMMU_DOMAIN_DMA | S1 | Synchronous (strict) |
| IOMMU_DOMAIN_DMA_FQ | S1 | Lazy (flush queue) |
| IOMMU_DOMAIN_UNMANAGED | S1 or S2 | Manual (VFIO) |
