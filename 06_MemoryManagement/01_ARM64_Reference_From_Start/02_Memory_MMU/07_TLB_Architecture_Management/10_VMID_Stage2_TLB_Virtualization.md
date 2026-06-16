# VMID and Stage 2 TLB: Virtualization TLB Architecture

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, AMD, NVIDIA, hypervisor teams

---

## 1. VMID Concept

```
VMID (Virtual Machine Identifier):
  Register: VTTBR_EL2[63:48] = VMID (up to 16 bits)
  
  Purpose: tag Stage 2 TLB entries with the current VM's identity
  Similar to ASID (for Stage 1 user processes), but at the hypervisor level
  
  Without VMID:
    Every context switch between VMs → full TLB flush
    Same overhead as context switch without ASID (catastrophic for VM density)
    
  With VMID:
    Each VM's TLB entries are tagged with its VMID
    Switch to another VM: change VMID in VTTBR_EL2 → no TLB flush needed!
    Different VMs' TLB entries coexist in TLB, tagged by VMID

VMID size:
  8-bit VMID: VTCR_EL2.VS = 0 → supports 255 concurrent VMs (VMID 0 = EL2 native)
  16-bit VMID: VTCR_EL2.VS = 1 → supports 65535 concurrent VMs
    (requires ARMv8.1 FEAT_VMID16)
    
  Detection:
    ID_AA64MMFR1_EL1.VMIDBits[7:4] = 0b0000 → 8-bit VMID
    ID_AA64MMFR1_EL1.VMIDBits[7:4] = 0b0010 → 16-bit VMID
    
  KVM ARM64 uses 8-bit or 16-bit based on system capability.
  Large cloud providers (100s of VMs per host): 16-bit VMID mandatory.
```

---

## 2. Stage 2 TLB Entry Structure

```
Combined TLB lookup for guest (EL1/EL0) in virtualized system:

Stage 1 translation (guest OS):
  VA (Guest Virtual Address) → GPA (Guest Physical Address = IPA)
  Uses: TTBR0_EL1 / TTBR1_EL1 (guest OS page tables)
  Tagged with: ASID (from TTBR0_EL1[63:48])

Stage 2 translation (hypervisor):
  GPA (Guest Physical Address) → HPA (Host Physical Address = real PA)
  Uses: VTTBR_EL2 (hypervisor Stage 2 page tables)
  Tagged with: VMID (from VTTBR_EL2[63:48])

Combined TLB entry:
  [VA] + [ASID] + [VMID] + [PPN (HPA)] + [S1 attrs] + [S2 attrs combined]
  
  TLB hit requires ALL three to match: VA + ASID + VMID
  Global entry (nG=0): VA + VMID must match (ASID ignored)
  
  Result: TLB holds a COMBINED VA→HPA entry
  No need to do Stage1 then Stage2 separately on TLB hit
  Only on TLB miss: hardware does full 2-level walk

Entry size (with Stage 2): approx 80–96 bits per entry:
  VA[47:12] = 36 bits
  ASID = 8 or 16 bits
  VMID = 8 or 16 bits
  PPN[47:12] = 36 bits
  Attributes (MAIR index, AP, PXN, UXN, AF, nG, shareability) = 24 bits
  Stage 2 MemAttr = 8 bits
  
  Combined entries are larger → virtualized TLB holds fewer entries
  TLB effective capacity drops when Stage 2 is active
```

---

## 3. TLBI Instructions for Stage 2

```
Stage 2 TLB invalidation (from EL2):

TLBI ALLE1IS:
  Invalidate ALL Stage 1 entries for ALL VMIDs
  (Nuclear option: use for VMID rollover or system reset)

TLBI VMALLS12E1IS:
  Invalidate ALL Stage 1 + Stage 2 entries for current VMID
  Used when: guest OS page table changes that affect Stage 2 too
  Common on: VM migration (guest pages moved to different HPAs)

TLBI IPAS2E1IS, Xt:
  Invalidate Stage 2 entry for GPA (IPA) in Xt
  Xt[47:12] = IPA >> 12
  Used when: hypervisor changes one GPA→HPA mapping (e.g., balloon driver removes page)
  Invalidates Stage 2 entry, forces re-walk on next access

TLBI ALLE2IS:
  Invalidate all EL2 TLB entries (hypervisor's own page table)
  Hypervisor code/data, not guest entries
  
DSB + ISB must follow TLBI (same rules as Stage 1):
  DSB ISH after TLBI to complete all CPUs' TLB invalidations
  ISB to prevent stale pipeline state after TLB change

KVM usage in kvm/arm64/mmu.c:
  kvm_flush_remote_tlbs_with_level():
    → Calls tlb_flush_vmid_range() which issues TLBI IPAS2E1IS per-IPA range
  
  kvm_tlb_flush_vmid():
    → Issues TLBI VMALLS12E1IS (flush all Stage1+2 for current VMID)
    → Called during VM reset or VMID recycling
```

---

## 4. VMID Rollover

```
VMID rollover: same as ASID rollover
  When all VMIDs exhausted (255 or 65535 VMs created/destroyed):
  
  Linux KVM VMID rollover (arch/arm64/kvm/arm.c):
    1. Atomic increment of vmid_generation counter
    2. TLBI ALLE1IS (flush ALL Stage 1+2 entries for ALL VMIDs)
    3. Reset all VM's vmid_generation to indicate stale VMID
    4. VMs that next become active get new VMID from new generation
    
  VMID state per KVM VM:
    kvm->arch.vmid.vmid: current VMID value (8 or 16 bits)
    kvm->arch.vmid.vmid_gen: generation when VMID was assigned
    
  On VM scheduling (kvm_arm_vmid_update()):
    If vmid_gen == current_vmid_generation:
      VMID still valid → just update VTTBR_EL2 and go
    Else:
      Need new VMID → allocate from bitmap, set new vmid_gen
      If no free VMIDs: trigger VMID rollover (TLBI ALLE1IS)
```

---

## 5. VHE (Virtualization Host Extensions) TLB

```
VHE (ARMv8.1 FEAT_VHE):
  Allows the Linux kernel to run at EL2 directly (instead of EL1)
  Purpose: eliminate EL1↔EL2 transitions for host kernel syscalls
  
  HCR_EL2.E2H = 1: enables VHE
  HCR_EL2.TGE = 1: guest takes EL1 traps to EL2 (host kernel acts as hypervisor)
  
  TLB impact with VHE:
    Host kernel (at EL2) uses TTBR0_EL2 (redirected to TTBR0_EL1) for host user pages
    Host kernel (at EL2) uses TTBR1_EL2 (redirected to TTBR1_EL1) for kernel pages
    
    Host kernel TLB entries tagged at EL2
    Guest VM TLB entries tagged at EL1 with ASID + VMID
    
  TLBI instructions in VHE:
    When HCR_EL2.E2H=1, TTBR0_EL1/TTBR1_EL1 access is redirected to EL2 registers
    TLBI VAE1IS: invalidates EL2 (host) AND EL1 (guest) entries at EL2
    This is intentional: VHE blurs EL1/EL2 TLB distinction
    
  pKVM (Protected KVM):
    Uses VHE for the hypervisor but enforces strong isolation
    Guest VMs cannot observe host kernel memory
    VMID strictly enforced, TLBI operations verified by pKVM hypervisor
    Available in Android 12+ as an option for stronger VM isolation
```

---

## 6. Interview Questions & Answers

**Q1: How does the TLB differentiate between two VMs that have the same guest virtual addresses mapped to the same guest physical addresses but different host physical addresses?**

The TLB uses **VMID** (Virtual Machine Identifier) stored in `VTTBR_EL2[63:48]` to tag Stage 2 TLB entries. A combined TLB entry holds `VA + ASID + VMID + HPA + attributes`. Even if two VMs (say VM-1 and VM-2) have identical guest VA→GPA mappings, they map to **different HPAs** via Stage 2 page tables. When VM-1's VMID is active, TLB entries tagged with VM-1's VMID are used; when VM-2 is scheduled with its different VMID, hardware only hits entries tagged with VM-2's VMID. The VMs' TLB entries coexist in the TLB simultaneously — no flush needed on VM context switch (unless VMID rollover occurs). This is the same principle as ASID for process isolation but applied at the VM granularity.

**Q2: What happens when VMID space is exhausted?**

When all VMID values are allocated (255 for 8-bit, 65535 for 16-bit VMIDs), KVM performs a **VMID rollover**: increments the generation counter atomically and issues `TLBI ALLE1IS` to flush **all** Stage 1 and Stage 2 TLB entries for all VMIDs across all CPUs. All existing VMID assignments are marked stale. As VMs get scheduled again, each is assigned a new VMID from the fresh generation. The flush is expensive (full TLB drain on all cores) but happens rarely — typically only when thousands of VMs have been created and destroyed. Using 16-bit VMIDs (`VTCR_EL2.VS=1`) pushes this out to 65535 allocations, making rollover essentially negligible in practice.

---

## 7. Quick Reference

| Register | Bits | Field | Purpose |
|---|---|---|---|
| VTTBR_EL2 | [63:48] | VMID | Current VM's identifier |
| VTTBR_EL2 | [47:1] | BADDR | Stage 2 page table base address |
| VTCR_EL2 | [19] | VS | 0=8-bit VMID, 1=16-bit VMID |
| HCR_EL2 | [0] | VM | 1=Stage 2 translation enabled |
| HCR_EL2 | [34] | E2H | 1=VHE mode |

| TLBI Instruction | Scope | Stage 1? | Stage 2? |
|---|---|---|---|
| TLBI ALLE1IS | All VMIDs | Yes | Yes |
| TLBI VMALLS12E1IS | Current VMID | Yes | Yes |
| TLBI IPAS2E1IS | Current VMID, one GPA | No | Yes |
| TLBI ALLE2IS | EL2 only | No (EL2 entries) | No |
