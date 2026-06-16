# Virtualization Memory: Complete Interview Reference

**Category**: Virtualization Memory and Stage 2 Translation  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Virtualization Memory Complete Summary

```
Memory translation hierarchy with KVM:

Bare metal (no hypervisor):
  VA ─────────────────────────────────────────────▶ PA
      [Stage 1: TTBR0_EL1/TTBR1_EL1 + TCR_EL1]

With KVM/ARM64 (HCR_EL2.VM=1):
  Guest VA ─────[Stage 1]────▶ IPA ─────[Stage 2]────▶ PA
                TTBR0_EL1               VTTBR_EL2

Key registers:
  HCR_EL2.VM  = 1: enable Stage 2 translation
  HCR_EL2.RW  = 1: guest runs AArch64
  HCR_EL2.E2H = 1: VHE mode (host kernel at EL2)
  VTTBR_EL2: [VMID[63:48] | S2_pgd_PA[47:1] | CnP[0]]
  VTCR_EL2:  [DS | NSW | NSA | VS | PS | TG0 | SH0 | ORGN0 | IRGN0 | SL0 | T0SZ]
  HPFAR_EL2: faulting IPA on Stage 2 fault (page-frame granularity)

Stage 2 fault handling call chain:
  [guest accesses unmapped IPA]
        │
  [Stage 2 translation fault]
        │
  [EL2 takes exception]
        │
  el2_sync() / el1_sync() (kvm entry)
        │
  handle_exit(vcpu)
        │
  kvm_handle_guest_abort(vcpu)
        │
  gfn_to_memslot() ─── find backing HVA
        │
  handle_mm_fault(hva) ─── allocate host page if needed
        │
  kvm_pgtable_stage2_map(ipa, pa, size, prot) ─── create S2 mapping
        │
  [resume guest vCPU]
```

---

## 2. VMID and TLB Management

```
VMID (Virtual Machine Identifier) management:
  VMID purpose: tag TLB entries with VM identity (no flush on VM switch)
  VMID size: 8-bit (256 unique) or 16-bit (65536 unique) per VTCR_EL2.VS
  
  VMID allocation (arch/arm64/kvm/vmid.c):
    kvm_vmid_alloc(): assign VMID from generation bitmap
    kvm_vmid_bump_generation(): wrap VMID space (starts new generation)
      → TLBI VMALLS12E1IS: flush all Stage1+Stage2 for all VMIDs
      → All VMs get new VMIDs on next entry
  
  VMID in VTTBR_EL2:
    Bits [63:48]: VMID (16-bit if VTCR_EL2.VS=1)
    Changing VMID: just write new VTTBR_EL2 → CPU uses new Stage 2 table
    Old TLB entries (with old VMID): don't match → naturally invalid

Stage 2 TLBI instructions:
  TLBI VMALLE1IS:    Invalidate all Stage 1 entries, current VMID, IS
  TLBI VMALLS12E1IS: Invalidate all Stage 1 + Stage 2, current VMID, IS
  TLBI IPAS2E1IS:    Invalidate Stage 2 entries by IPA, IS
                     Used: when Stage 2 PTE removed (kvm_pgtable_stage2_unmap)
  TLBI IPAS2LE1IS:   Same but only leaf (page, not block) entries
  TLBI ALLE2IS:      Invalidate all EL2 entries (for hypervisor use)
  TLBI ALLE12IS:     Invalidate all EL1+EL0 Stage 1+2 for all VMIDs

Correct TLB invalidation sequence for Stage 2 unmap:
  str xzr, [pte_addr]          // clear Stage 2 PTE
  dsb ishst                    // ensure PTE write is visible
  tlbi ipas2le1is, ipa_shifted // invalidate by IPA (ipa >> 12 in x0[51:0])
  dsb ish                      // wait for TLBI completion across all PEs
  isb                          // ensure subsequent accesses use new translation

KVM function: kvm_tlb_flush_vmid_ipa():
  Executes: TLBI IPAS2E1IS (or IPAS2LE1IS) for the specific IPA
  Called by: kvm_pgtable_stage2_unmap_walker()
```

---

## 3. Live Migration Memory Tracking

```
Live migration: move running VM from host A to host B
  Challenge: track which VM memory pages were written ("dirty") during migration
  Solution: Stage 2 dirty page tracking via write protection
  
  Phase 1: initial copy
    Copy all VM RAM from host A to host B (background)
    Mark ALL Stage 2 pages as read-only (write-protect)
    
  Phase 2: dirty tracking
    Guest writes to page → Stage 2 permission fault (write to read-only)
    kvm_handle_guest_abort(): fault_status = PERMISSION_FAULT
    KVM: set dirty bit in kvm_dirty_bitmap[gfn]
    KVM: re-map page as read-write (resume guest)
    
  Phase 3: final sync
    Stop guest vCPU briefly
    Copy only dirty pages from A to B
    Start guest on host B
  
  ARM64 FEAT_HAFDBS: Hardware Access/Dirty Bit Support
    Hardware automatically sets AF (access flag) and DBM (dirty bit)
    in Stage 2 PTEs on first access/write
    No need for software write-protection → better performance
    
    DBM bit in Stage 2 entry:
      HW: when write occurs to DBM=1 entry: sets hardware dirty bit (S2AP[1] cleared)
      Software: scan PTEs looking for cleared S2AP[1] → dirty page
      Clear: restore S2AP[1]=1 (after copying page to destination)
    
    Linux KVM: kvm_pgtable_stage2_set_owner():
      if (has_hafdbs): enable DBM in Stage 2 entries for dirty tracking
      → more efficient than software write-protection

ARM64 KVM dirty ring (FEAT_DIRTY_LOG_RING):
  New mechanism: VCPU dirty ring buffer
  Instead of: per-VM bitmap (large, must scan)
  VCPU has ring buffer: records (gfn, slot_offset) for each dirty page
  kvm_arch_vcpu_ioctl_get_dirty_log(): drain dirty ring to userspace
  QEMU: iterates dirty ring, copies pages to destination
  Benefit: O(dirty pages) vs O(all pages) for bitmap scan
```

---

## 4. Top 15 Interview Questions

**Q1: What is IPA (Intermediate Physical Address) in ARM64 virtualization?**
IPA is what the guest OS thinks is physical memory. From the guest's perspective, it allocates and uses physical frames starting at 0. The guest programs its Stage 1 page tables using IPA as "physical addresses". The hypervisor maps IPA → real PA via Stage 2 translation. The guest never knows the real PA.

**Q2: How is VMID different from ASID?**
ASID (Address Space Identifier) tags Stage 1 TLB entries within one VM (distinguishes processes). VMID tags Stage 2 TLB entries (distinguishes VMs). An ARM64 TLB entry is tagged with both: {VMID, ASID, VA} → PA. Changing VMID (switching VMs) invalidates Stage 2 without affecting entries from other VMs.

**Q3: What is the Stage 2 page table format on ARM64?**
Same AArch64 long-descriptor format as Stage 1, with differences: S2AP (access permissions) instead of AP; MemAttr[3:0] directly encoded (no MAIR index); no UXN/PXN (execute-never handled by XN bit); VMID in VTTBR_EL2 instead of ASID in TTBR. Supports 1GB, 2MB, and 4KB granularity in one table.

**Q4: How does KVM handle a Stage 2 translation fault?**
1. Guest accesses IPA with no Stage 2 mapping → fault to EL2. 2. `kvm_handle_guest_abort()` reads `HPFAR_EL2` (faulting IPA) and `ESR_EL2` (fault type). 3. Finds QEMU memslot: `gfn_to_memslot(kvm, ipa >> PAGE_SHIFT)`. 4. Gets host virtual address: `gfn_to_hva(kvm, gfn)`. 5. Calls `handle_mm_fault(hva)` to allocate host page if needed. 6. Maps: `kvm_pgtable_stage2_map(ipa, pa, size)`. 7. Resumes guest vCPU.

**Q5: What is VHE and how does it improve KVM performance?**
VHE (Virtualization Host Extensions, ARMv8.1) allows the host kernel to run at EL2. Without VHE: host kernel at EL1, hypervisor code at EL2 — VM entry/exit crosses EL1↔EL2 boundary twice. With VHE: host kernel runs at EL2, no EL1↔EL2 transitions for host operations. Register aliasing: EL1 system register names redirect to EL2 registers. Net result: fewer exception transitions per VM entry/exit.

**Q6: Explain pKVM's protection model.**
pKVM separates a small, trusted EL2 hypervisor from the large, untrusted host kernel. pKVM controls Stage 2 tables for both host Linux and VMs. When memory is donated to a VM, pKVM removes it from the host's Stage 2 — the host kernel cannot access it even if compromised. Memory is zeroed before returning to host. Requires EL2 privilege to modify Stage 2 — host kernel at EL1 cannot bypass this.

**Q7: How does dirty page tracking work for VM live migration?**
Write-protection approach: Mark all guest pages in Stage 2 as read-only. On guest write: Stage 2 permission fault → KVM marks page in dirty bitmap, re-maps as read-write. Final sync: copy only dirty pages. Modern approach: FEAT_HAFDBS sets a dirty bit in the Stage 2 PTE directly (no software write-protection needed). `VCPU Dirty Ring`: VCPU maintains a ring buffer of dirty GFNs (more efficient than bitmap scan).

**Q8: What registers does the hypervisor configure for a new VM?**
VTTBR_EL2 (Stage 2 page table base + VMID), VTCR_EL2 (IPA size, granule, cacheability), HCR_EL2 (enable Stage 2, set guest mode, configure traps), VBAR_EL1 (guest vector table location), SCTLR_EL1 (guest system control — initially with MMU disabled until guest enables it).

**Q9: How does FEAT_HAFDBS help KVM?**
FEAT_HAFDBS (Hardware Access Flag and Dirty State) allows the CPU to automatically set the Access Flag (AF) and dirty bit (DBM/DBMS) in Stage 2 page table entries without taking a fault. Without it: KVM must use write-protection + permission faults to detect dirty pages (very expensive for write-heavy workloads). With FEAT_HAFDBS: hardware sets dirty bit directly → software just scans PTEs for set dirty bits. Supported on Neoverse N2/V2/N3.

**Q10: What is the TLB invalidation sequence when removing a Stage 2 mapping?**
1. Clear Stage 2 PTE: `str xzr, [pte_entry]`. 2. `DSB ISHST`: ensure PTE write is visible before TLBI. 3. `TLBI IPAS2LE1IS, <ipa>>12>`: invalidate Stage 2 TLB by IPA (Inner Shareable broadcast). 4. `DSB ISH`: wait for TLB invalidation to complete on all CPUs. 5. `ISB`: ensure subsequent memory accesses use new (invalidated) translations. Only after step 5: safe to free the physical page.

**Q11: How many memory reads does a worst-case ARM64 Stage 1+2 combined walk take?**
For 48-bit VA with 4KB granule (4-level tables: L0/L1/L2/L3), each Stage 1 pointer lookup also needs a Stage 2 translation (L0/L1/L2/L3). Worst case: 4 Stage 1 reads × 4 Stage 2 reads = 16 reads. Plus the final PA check through Stage 2 = 4 more = 20 total reads. In practice: L0 tables are usually contiguous → ARM hardware optimization reduces this significantly. TLB caches the final result.

**Q12: What is HPFAR_EL2 and why is it needed?**
`HPFAR_EL2` (Hypervisor Physical Fault Address Register) contains the page-frame-aligned IPA that caused a Stage 2 fault. Without it: the hypervisor would only know the VA (from `FAR_EL2`) — it would need to walk the guest's Stage 1 page tables (which are in IPA space!) to find the faulting IPA, which would itself require Stage 2 translation. `HPFAR_EL2` short-circuits this: hardware directly provides the IPA, saving multiple page table walks per fault.

**Q13: Can a guest OS detect that it's running inside a VM on ARM64?**
A guest can infer virtualization by: (1) reading `ID_AA64MMFR1_EL1.VMIDBits` — indicates whether VMID is supported (available at EL1); (2) accessing EL1 system registers that would trap to EL2 (HCR_EL2.TIDCP, HCR_EL2.TPCP); (3) timing page faults (Stage 1+2 combined fault slower than Stage 1 only); (4) checking `SMBR_EL1` for software managed broadcast. Hypervisors often emulate ID registers to hide virtualization from guest for compatibility.

**Q14: How does KVM handle VM memory hotplug?**
Guest requests memory hot-add → ACPI event or virtio balloon. KVM: adds new `kvm_memory_slot` with the new IPA range. No Stage 2 mappings created initially. Guest accesses new memory → Stage 2 translation fault → `kvm_handle_guest_abort()` allocates host pages, creates Stage 2 mappings. Memory hot-remove: more complex — guest must first remove all references, then KVM calls `kvm_pgtable_stage2_unmap()` + `TLBI IPAS2LE1IS` before freeing host pages.

**Q15: What is the difference between TLBI IPAS2E1IS and TLBI VMALLS12E1IS?**
`TLBI IPAS2E1IS`: invalidate Stage 2 TLB entries matching a specific IPA. Precise — only removes the one (or few) entries for the given IPA. Used when unmapping a specific page. `TLBI VMALLS12E1IS`: invalidate ALL Stage 1 AND Stage 2 entries for the current VMID. Blunt instrument — used when destroying a VM, or when VMID wraps around (all current VMID entries must be cleared). The former is O(1 entry), the latter is O(all entries).

---

## 5. Quick Reference

| Register | Role | Who Writes |
|---|---|---|
| VTTBR_EL2 | Stage 2 table base + VMID | KVM EL2 |
| VTCR_EL2 | Stage 2 config | KVM EL2 |
| HCR_EL2 | Hypervisor config | KVM EL2 |
| HPFAR_EL2 | Faulting IPA (read-only) | Hardware |
| ESR_EL2 | Fault syndrome | Hardware |
| FAR_EL2 | Faulting VA | Hardware |
| ELR_EL2 | Return address (guest PC) | Hardware |

| Stage 2 Huge Page | Coverage | When Used |
|---|---|---|
| 1GB block (L1) | 1GB per entry | RAM regions, large device BAR |
| 2MB block (L2) | 2MB per entry | Most VM RAM (good balance) |
| 4KB page (L3) | 4KB per entry | Fine-grained / partial regions |

| Virtualization Feature | ARM Version | Purpose |
|---|---|---|
| Stage 2 translation | ARMv8.0 | VM memory isolation |
| VHE | ARMv8.1 | Host kernel at EL2 |
| VMID 16-bit | ARMv8.1 | More VMs in TLB |
| FEAT_HAFDBS | ARMv8.1 | HW dirty tracking |
| FEAT_LPA | ARMv8.2 | 52-bit PA |
| FEAT_DOUBLEFAULT | ARMv8.4 | Better error reporting |
| FEAT_RME/CCA | ARMv9.2 | Realm VMs (hardware isolation) |
