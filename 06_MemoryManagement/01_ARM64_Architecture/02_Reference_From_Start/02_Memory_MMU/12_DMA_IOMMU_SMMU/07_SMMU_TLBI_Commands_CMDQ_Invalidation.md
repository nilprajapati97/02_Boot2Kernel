# SMMU TLBI Commands and CMDQ Invalidation

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
SMMU TLB (IOTLB): caches IOVA → PA translations
  Without IOTLB: every device DMA → page table walk → memory latency
  With IOTLB: most DMA → fast single-cycle lookup
  
  Problem: when Linux changes SMMU page tables (map/unmap/remap),
           stale IOTLB entries must be invalidated!
           
  Failure to invalidate IOTLB:
    Device still uses old IOVA → PA mapping
    After page is unmapped: device accesses freed page → wrong physical page
    After remap: device reads old data → data corruption
    After protection change: device may still write to now-read-only memory
    
  IOTLB invalidation: ARM SMMU v3 uses CMDQ (Command Queue) ring buffer

Comparison: CPU TLB vs SMMU IOTLB invalidation:

  CPU TLB:
    Instructions: TLBI VAE1IS, TLBI ASID1, TLBI VMALLE1IS (broadcast)
    ISB: ensures new translations used after this instruction
    DSB: ensures observation by all PEs (for broadcast invalidation)
    Immediate: TLB flush completes when ISB finishes
    Synchronous: the executing CPU waits
    
  SMMU IOTLB (v3):
    Commands: CMD_TLBI_NH_VA, CMD_TLBI_NH_ASID, etc. (see below)
    Posted to CMDQ ring buffer (normal memory write)
    Asynchronous: SMMU hardware reads and processes CMDQ independently
    Synchronization: CMD_SYNC: SMMU signals completion
    Software polls CMDQ cons_ptr OR waits for sync IRQ
```

---

## 2. SMMUv3 CMDQ Architecture

```
CMDQ (Command Queue): ring buffer in normal (cacheable) memory

Layout:
  CMDQ.BASE: base physical address (written to SMMU register SMMU_CMDQ_BASE)
  CMDQ.BASE.LOG2SIZE: queue depth (e.g., 7 = 128 entries = 128 × 16 bytes)
  SMMU_CMDQ_PROD: producer pointer register (written by SW)
  SMMU_CMDQ_CONS: consumer pointer register (written by SMMU HW)
  
  Ring buffer:
    [CMD[0]][CMD[1]][CMD[2]]...[CMD[127]]
    CONS ──────────────────────────────▶
    PROD ──────────────────────────────────────▶
    (SMMU reads from CONS, software writes to PROD)
    
  Full condition: (PROD + 1) % size == CONS
  Empty condition: PROD == CONS

Each command: 16 bytes (2 × 64-bit words)
  Word 0, bits [7:0]: OPCODE (command type)

Software posting a command (arm_smmu_cmdq_issue_cmd()):
  1. spin_lock(CMDQ.lock): serialize multiple CPUs posting commands
  2. while (CMDQ_FULL): spin_until_space_available()
  3. copy command to: CMDQ.BASE + (prod_ptr % size) * 16
  4. dsb(ishst):  ensure command write is visible to SMMU before updating PROD
  5. writel(new_prod, SMMU_CMDQ_PROD): SMMU sees new command
  6. spin_unlock(CMDQ.lock)

SMMU processing commands:
  Hardware reads commands from CONS pointer
  Processes: TLBI, CFGI, SYNC, etc.
  Updates CONS register after processing each command
  On CMD_SYNC: sends completion signal (IRQ or sets status bit)
```

---

## 3. TLBI Command Reference

```
CMD_TLBI_NH_ALL: Invalidate ALL non-hypervisor (NH) TLB entries
  Format:
    Word 0: OPCODE=0x10
    No additional parameters
  
  Use: destroy an iommu_domain (unmap entire device address space)
       or arm_smmu_flush_iotlb_all() called when domain is freed
  
  Cost: very expensive — flushes entire SMMU IOTLB
        All devices with this SMMU: all translations must be re-walked
        Use sparingly!

CMD_TLBI_NH_ASID: Invalidate all TLB entries for one ASID
  Format:
    Word 0: OPCODE=0x11
    Word 0[63:48]: ASID (16-bit)
  
  Use: iommu_domain freed, or large range unmap for one device
       Linux: arm_smmu_tlb_inv_context_s1(cookie):
         Issue CMD_TLBI_NH_ASID(asid) + CMD_SYNC

CMD_TLBI_NH_VA: Invalidate TLB entries by VA (range)
  Format:
    Word 0: OPCODE=0x12
    Word 0[63:48]: ASID
    Word 0[31]:    Leaf-only flag (only leaf entries, not intermediate)
    Word 1[63:12]: IOVA (4KB aligned)
    Word 1[47:20]: Range (number of pages, FEAT_TLBIRANGE)
  
  Use: iommu_unmap(domain, iova, size) for small ranges
       Most efficient: only flushes affected IOTLB entries

CMD_TLBI_S2_IPA: Invalidate Stage 2 TLB by IPA
  Format:
    Word 0: OPCODE=0x14
    Word 0[47:32]: VMID
    Word 1[63:12]: IPA
  
  Use: hypervisor changes Stage 2 page tables (VM memory remapping)
       arm_smmu_tlb_inv_range_s2(): called when VM page tables change

CMD_TLBI_S12_VMALL: Invalidate ALL Stage 1 + Stage 2 for one VMID
  Format:
    Word 0: OPCODE=0x16
    Word 0[47:32]: VMID
  
  Use: VM is destroyed (all device DMA mappings for that VM invalidated)

CMD_TLBI_EL2_ALL: Invalidate EL2 TLB entries (for EL2/hypervisor use)
  Use: hypervisor IOMMU domain flush in pKVM / Xen

FEAT_TLBIRANGE (ARM SMMUv3.1+):
  Adds range fields to TLBI commands (Word 1 bits [47:20])
  Without FEAT_TLBIRANGE: must issue one CMD_TLBI_NH_VA per page
  With FEAT_TLBIRANGE: single command covers entire range
  Linux checks: smmu->features & ARM_SMMU_FEAT_RANGE_INV
```

---

## 4. Configuration Invalidation Commands

```
SMMU also caches STE and CD entries:
  After modifying STE/CD: MUST invalidate config cache!
  
CMD_CFGI_STE: Invalidate STE (Stream Table Entry) cache
  Format:
    Word 0: OPCODE=0x03
    Word 0[63:32]: StreamID
    Word 0[0]: Leaf (0 = this STE only, 1 = also invalidate cached CDT)
  
  Use: any time STE is modified:
    - Device attached to new domain (STE.S1ContextPtr changes)
    - Stage 2 table changed (STE.S2TTB changes)
    - Config field changed (S1→S2 or bypass→translate)
  
  Linux: arm_smmu_write_strtab_ent() →
           arm_smmu_cmdq_issue_cmd(CMD_CFGI_STE(sid, leaf=1))

CMD_CFGI_ALL: Invalidate all STE cache entries
  Use: SMMU driver initialization / recovery
  
CMD_CFGI_CD: Invalidate CD (Context Descriptor) cache
  Format:
    Word 0: OPCODE=0x05
    Word 0[63:32]: StreamID
    Word 0[31:20]: SubstreamID (PASID)
    Word 0[0]: Leaf
  
  Use: CD modified (page table change, ASID change, per-PASID update)
  Linux: arm_smmu_write_ctx_desc() → CMD_CFGI_CD(sid, ssid, leaf)

CMD_SYNC: Synchronize command completion
  Format:
    Word 0: OPCODE=0x46
    Word 0[1:0]: CS (completion signaling): 0=poll, 1=MSI, 2=poll+stall
  
  Use: ALWAYS after issuing TLBI/CFGI commands before assuming completion
  Linux: arm_smmu_cmdq_issue_cmd(CMD_SYNC) → poll CMDQ cons_ptr
  
  ARM SMMU v3 spec: CMD_SYNC with CS=0 (no MSI):
    SMMU writes MSH (memory shareability) to SYNC MSI data
    Software polls MSI data location until it equals expected value
    OR: poll until CMDQ.cons_ptr advances past SYNC command
  
  Critical: without CMD_SYNC, later DMA may still hit stale IOTLB entry!
            timing: SMMU may process SYNC out-of-order (out-of-order queue)
            SYNC guarantees: all prior commands ARE completed before SYNC completes
```

---

## 5. ARM64 CPU TLBI vs SMMU TLBI Comparison

```
CPU TLBI (e.g., TLBI VAE1IS):
  Issued as: ARM64 instruction (system register write)
  Broadcast: automatic (ISH domain = all CPUs see it)
  Completion: ISB ensures CPU uses new translation after ISB
  DSB before TLBI: ensures page table write is visible first
  DSB after TLBI + ISB: ensures TLB invalidation is complete
  Example:
    str xzr, [pte_addr]          // clear PTE
    dsb ishst                    // ensure PTE write visible
    tlbi vae1is, x0              // invalidate by VA
    dsb ish                      // wait for TLB invalidation
    isb                          // use new translation

SMMU TLBI (CMD_TLBI_NH_VA + CMD_SYNC):
  Issued as: write to CMDQ in memory
  Processed: asynchronously by SMMU hardware
  Completion: CMD_SYNC + poll (or MSI IRQ)
  dsb(ishst) before prod update: ensures command visible to SMMU
  Example:
    smmu_map_remove(pte)         // clear SMMU PTE
    arm_smmu_cmdq_issue_cmd(CMD_TLBI_NH_VA(...))
    arm_smmu_cmdq_issue_cmd(CMD_SYNC)
    arm_smmu_cmdq_poll_until_consumed()  // wait for completion

Key differences:
  CPU TLBI: synchronous instruction, broadcast to all CPUs automatically
  SMMU TLBI: asynchronous ring buffer, SMMU hardware reads independently
  CPU TLBI range: FEAT_TLBRANGE (ARMv8.4) adds range support
  SMMU TLBI range: FEAT_TLBIRANGE (SMMUv3.1) adds range support
  
Both must complete BEFORE:
  Freeing the physical page
  Changing page attributes
  Passing memory to a different process/VM
```

---

## 6. Interview Questions & Answers

**Q1: What happens if software forgets to issue CMD_SYNC after CMD_TLBI_NH_VA?**

The SMMU Command Queue is processed asynchronously by the SMMU hardware. When software writes `CMD_TLBI_NH_VA` and immediately proceeds without `CMD_SYNC`:

1. SMMU hardware MAY still have the stale IOTLB entry cached
2. A device DMA that arrives microseconds after the TLBI command is posted may find the old (now-invalid) IOTLB entry and use the OLD physical address
3. If the old physical page has already been freed and given to another process: **memory corruption** — the device writes into another process's memory!

`CMD_SYNC` is the synchronization barrier: SMMU guarantees that all commands issued BEFORE the SYNC are completed BEFORE the SYNC itself completes. Software polling the CMDQ cons_ptr (waiting for it to advance past the SYNC) ensures:
- All TLBI commands before SYNC: definitely completed
- New device DMA after SYNC completion: will hit IOTLB miss → do fresh page walk → find correct (or absent) mapping

This is exactly why `arm_smmu_iotlb_sync()` always issues TLBI + SYNC as a pair.

**Q2: Why does ARM SMMUv3 use a ring buffer (CMDQ) instead of direct MMIO register writes like SMMUv2?**

SMMUv2 uses dedicated MMIO registers for TLB invalidation:
- `TLBIALL`: write to invalidate all
- `TLBIVMID`: write VMID to invalidate  
- `TLBIVA`: write VA + ASID

Problems with SMMUv2 MMIO approach:
1. **Sequential**: each MMIO write is serialized — device-memory write (non-cacheable, non-reorderable)
2. **No batching**: cannot queue multiple TLBIs and process them in parallel
3. **Synchronous**: software must poll completion register after every TLBI
4. **Scalability**: at 100K IOMMU map/unmap operations/second, synchronous MMIO is the bottleneck

SMMUv3 CMDQ advantages:
1. **Batching**: write 10 TLBIs then 1 SYNC → SMMU processes all 10 in one sweep
2. **Caching**: CMDQ is in normal cacheable memory → CPU cache benefits for repeated writes
3. **Parallelism**: software can post commands from multiple CPUs concurrently (with proper locking on PROD pointer)
4. **Async processing**: SMMU processes commands while software continues other work
5. **Scalability**: modern servers with 1000s of PCIe devices benefit enormously

Performance result: SMMUv3 CMDQ allows 10–100× more TLBIs per second vs SMMUv2 MMIO register approach.

---

## 7. Quick Reference

| SMMU TLBI Command | Scope | Use When |
|---|---|---|
| CMD_TLBI_NH_ALL | All IOTLB entries | Domain destroyed |
| CMD_TLBI_NH_ASID | One ASID all VA | Context/device removed |
| CMD_TLBI_NH_VA | One VA range, one ASID | Pages unmapped |
| CMD_TLBI_S2_IPA | One IPA, one VMID | VM page table change |
| CMD_TLBI_S12_VMALL | All S1+S2 for VMID | VM destroyed |

| SMMU Config Command | Use When |
|---|---|
| CMD_CFGI_STE | STE modified (device attached/detached) |
| CMD_CFGI_CD | CD modified (PASID change, page table change) |
| CMD_CFGI_ALL | SMMU init or full reset |
| CMD_SYNC | Always after TLBI/CFGI (completion sync) |

| vs CPU TLBI | CPU (e.g., TLBI VAE1IS) | SMMU (CMD_TLBI_NH_VA) |
|---|---|---|
| Mechanism | ARM64 instruction | Write to CMDQ ring buffer |
| Broadcast | Auto (ISH domain) | Not needed (SMMU is one device) |
| Completion | DSB + ISB | CMD_SYNC + poll |
| Range support | FEAT_TLBRANGE (ARMv8.4) | FEAT_TLBIRANGE (SMMUv3.1) |
