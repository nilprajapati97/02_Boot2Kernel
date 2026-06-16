# ARM32 Virtualization, Stage-2 Translation, and KVM
## Document 9: Hyp Mode, Two-Stage Translation, KVM/ARM, pKVM

**Author:** Senior Kernel Engineer  
**Target:** ARMv7-A (ARM32) with Virtualization Extensions, Linux KVM v5.x/v6.x  
**Scope:** PL2/Hyp mode, LPAE Stage-2, VMID, KVM/ARM, pKVM, SMMU Stage-2  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview), Document 03 (Linux MM Internals), Document 07 (SMMU/IOMMU)

---

## Table of Contents
1. [ARM Virtualization Extensions Overview](#1-arm-virtualization-extensions-overview)
2. [PL2 (Hyp) Mode — Register State](#2-pl2-hyp-mode--register-state)
3. [Two-Stage Address Translation](#3-two-stage-address-translation)
4. [LPAE (Large Physical Address Extension)](#4-lpae-large-physical-address-extension)
5. [VMID and TLB Management](#5-vmid-and-tlb-management)
6. [HVC / World Switch to Hypervisor](#6-hvc--world-switch-to-hypervisor)
7. [KVM/ARM Architecture](#7-kvmarm-architecture)
8. [KVM MMU — Stage-2 Page Tables](#8-kvm-mmu--stage-2-page-tables)
9. [KVM Guest Abort Handling](#9-kvm-guest-abort-handling)
10. [SMMU Stage-2 — Device Passthrough to VMs](#10-smmu-stage-2--device-passthrough-to-vms)
11. [Protected KVM (pKVM) — Google's Hypervisor](#11-protected-kvm-pkvm--googles-hypervisor)
12. [Performance and Trade-offs](#12-performance-and-trade-offs)

---

## 1. ARM Virtualization Extensions Overview

### 1.1 What ARM Virtualization Extensions Add

```
ARMv7-A without Virtualization Extensions:
  PL0: User mode (applications)
  PL1: OS mode (kernel, FIQ/IRQ/Abort/Undefined/SVC)
  PL2: Secure Monitor (TrustZone)
  
ARMv7-A WITH Virtualization Extensions (VE):
  PL0: User mode (applications inside VM or host)
  PL1: OS mode (guest kernel or host kernel)
  PL2: Hypervisor mode (Hyp) — NON-SECURE ONLY
  Secure PL0/PL1: TrustZone secure world
  Secure PL3: Secure Monitor

  Key additions:
    - Hyp mode (PL2): new privilege level for hypervisor
    - Two-stage address translation: VA → IPA → PA
    - VMID: 8-bit identifier to tag TLB entries per VM
    - VTTBR/VTCR: Stage-2 translation registers
    - HVC instruction: Hypercall (like SVC but targets Hyp mode)
    - Hypervisor Configuration Register (HCR): control trapping
```

### 1.2 Privilege Level Architecture with Virtualization

```
┌──────────────────────────────────────────────────────────────────┐
│                     Non-Secure World                             │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  PL0: Guest App / Host App                              │    │
│  │    (VA → Stage-1 by guest kernel → Stage-2 by hyp)     │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  PL1: Guest Kernel / Host Kernel                        │    │
│  │    Controls Stage-1 (TTBR0/TTBR1/TTBCR)               │    │
│  │    Stage-1: VA → IPA (guest kernel thinks this is PA)  │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  PL2: Hypervisor (KVM, Xen, etc.)                       │    │
│  │    Controls Stage-2 (VTTBR/VTCR)                       │    │
│  │    Stage-2: IPA → PA (hypervisor enforces isolation)   │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                      Secure World                                │
│  Secure PL0/PL1: TrustZone TEE (OP-TEE, QSEE, Trusty)         │
│  PL3 (Monitor mode): TF-A Monitor (TrustZone switch)           │
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. PL2 (Hyp) Mode — Register State

### 2.1 Banked Registers in Hyp Mode

```
PL2 has its own banked registers (like other ARM modes):

PL2-specific registers:
  HVBAR   (Hyp Vector Base Address Register)
  HDFAR   (Hyp Data Fault Address Register)
  HIFAR   (Hyp Instruction Fault Address Register)
  HPFAR   (Hyp IPA Fault Address Register — IPA of faulting access)
  HSR     (Hyp Syndrome Register — fault cause)
  HSTR    (Hyp System Trap Register — trap CP15 accesses to hyp)
  HCR     (Hypervisor Configuration Register — critical for trapping)
  HACTLR  (Hyp Auxiliary Control Register)
  HTCR    (Hyp Translation Control Register — for Hyp-mode VA→PA)
  HTTBR   (Hyp Translation Table Base Register — Hyp PGD)
  VTTBR   (Virtualization Translation Table Base Register — Stage-2 PGD)
  VTCR    (Virtualization Translation Control Register — Stage-2 config)
  VMID    (Virtual Machine ID — 8-bit, in VTTBR[55:48])

Shared registers: SP, LR (banked per mode)
  SP_hyp, LR_hyp — hypervisor stack and link register
  SPSR_hyp — saved program status from mode that triggered hyp entry
```

### 2.2 HCR — Hypervisor Configuration Register

```c
/* HCR bits — control what triggers VM exit to hypervisor */

HCR_VM    (1 << 0)   /* Enable Stage-2 translation */
HCR_SWIO  (1 << 1)   /* Set/Way ops trap (cache by set/way → IPA) */
HCR_PTW   (1 << 2)   /* Protected Table Walk: abort on non-permitted walk */
HCR_FMO   (1 << 3)   /* FIQ mask override: FIQ taken to Hyp */
HCR_IMO   (1 << 4)   /* IRQ mask override: IRQ taken to Hyp */
HCR_AMO   (1 << 5)   /* Async abort override: taken to Hyp */
HCR_VF    (1 << 6)   /* Virtual FIQ (inject FIQ into guest) */
HCR_VI    (1 << 7)   /* Virtual IRQ (inject IRQ into guest) */
HCR_VSE   (1 << 8)   /* Virtual SError abort */
HCR_FB    (1 << 9)   /* Force broadcast: TLB/cache ops broadcast */
HCR_BSU   (3 << 10)  /* Barrier Shareability Upgrade */
HCR_DC    (1 << 12)  /* Default Cacheable */
HCR_TWI   (1 << 13)  /* Trap WFI instruction to Hyp */
HCR_TWE   (1 << 14)  /* Trap WFE instruction to Hyp */
HCR_TID   (3 << 15)  /* Trap ID register accesses */
HCR_TSC   (1 << 19)  /* Trap SMC instruction to Hyp */
HCR_TIDCP (1 << 20)  /* Trap implementation-defined functionality */
HCR_TAC   (1 << 21)  /* Trap ACTLR */
HCR_TSW   (1 << 22)  /* Trap Set/Way cache maintenance */
HCR_TVM   (1 << 26)  /* Trap virtual memory controls */

/* Linux KVM setup (typical): */
HCR_VALUE = HCR_VM | HCR_IMO | HCR_FMO | HCR_AMO | HCR_SWIO | HCR_TSC;
```

---

## 3. Two-Stage Address Translation

### 3.1 Complete Translation Flow

```
Guest process issues load/store to virtual address VA:

Stage 1 (guest controls, PL1):
  VA (32-bit) → IPA (Intermediate Physical Address)
  Uses: TTBR0/TTBR1 (guest page tables)
        TTBCR.N for TTBR0/TTBR1 split
        Same short/long descriptor format as non-virtualized ARM32
  Result: IPA (guest thinks this is PA, but hypervisor intercepts)
  
Stage 2 (hypervisor controls, PL2):
  IPA → PA (real physical address)
  Uses: VTTBR (Stage-2 page table base)
        VTCR (Stage-2 translation control)
        LPAE format (must use LPAE for Stage-2, no short descriptor)
  Result: PA (actual DRAM location)

Combined:
  VA ──[S1: guest TTBR0]──→ IPA ──[S2: VTTBR]──→ PA
  
TLB entry when both stages active:
  {VMID, ASID, VA} → PA  (cached combined result, avoids double walk)
  
Fault handling:
  S1 fault: taken to guest OS (data abort in PL1)
            HSR.EC shows Stage-1 fault, HDFAR has VA
  S2 fault: taken to hypervisor (data abort in PL2)
            HSR.EC shows Stage-2 fault, HDFAR has IPA
            HPFAR has IPA >> 4 (faulting IPA)
```

### 3.2 Stage-2 Page Table Layout (LPAE)

```
LPAE (Long Descriptor format) — REQUIRED for Stage-2:

VA space: 32-bit (4GB) for guest
IPA space: determined by VTCR.T0SZ (default 32-bit)
PA space: up to 40-bit with LPAE (1TB physical memory)

Stage-2 table structure (3-level for 32-bit IPA):

Level 1 (L1): 4 entries (2-bit index from IPA[31:30])
  Each entry covers 1GB of IPA
  Entry: Block descriptor (1GB) or Table pointer to L2

Level 2 (L2): 512 entries (9-bit index from IPA[29:21])
  Each entry covers 2MB of IPA
  Entry: Block descriptor (2MB) or Table pointer to L3

Level 3 (L3): 512 entries (9-bit index from IPA[20:12])
  Each entry covers 4KB of IPA
  Entry: Page descriptor (4KB)

VTTBR format (64-bit):
  [63:56] VMID (8-bit Virtual Machine ID)
  [55:0]  Physical address of L1 table (4096-byte aligned → 12 PA bits = 0)
          Actually: IPA of L1 table (it IS PA in Stage-2 context)
```

### 3.3 Stage-2 Descriptor Format (LPAE)

```
Stage-2 Block/Page descriptor (64-bit):

[63:52]  Upper block attributes
  [63]    XN (Execute Never)
  [62]    Contiguous
  [52]    DBM (Dirty Bit Modifier, ARMv8.1+)

[51:30]  Upper PA bits (for 40-bit PA)
[29:12]  Block PA (for 2MB block) or
[29:21]  Block PA (for 1GB block)

[11:2]   Lower block attributes:
  [11:10] SH   (Shareability: 0b11=Inner shareable for SMP)
  [9:8]   AF+AP fields
  [7:6]   S2AP (Stage-2 access permissions):
              00: No access
              01: Read-only
              10: Write-only
              11: Read/Write
  [5:2]   MemAttr (Memory type: 0000=Device, 1111=Normal WB)

[1:0]    Type:
  11: Page/Block
  01: Table (L1/L2 only)
  00/10: Invalid/fault
```

---

## 4. LPAE (Large Physical Address Extension)

### 4.1 Why LPAE for Virtualization

```
ARM32 short descriptor (Section/Page):
  Physical address: 32-bit (max 4GB PA)
  Can NOT be used for Stage-2 translation tables
  
ARM32 LPAE (Long Descriptor):
  Physical address: up to 40-bit (1TB PA)
  REQUIRED for Stage-2 page tables (KVM mandates LPAE)
  Optional for Stage-1 (Linux uses LPAE when CONFIG_ARM_LPAE=y)
  
VTCR register:
  VTCR.T0SZ: Size of IPA space (2^(32-T0SZ) bytes)
  VTCR.SL0:  Starting level for Stage-2 walk
             0 = start at level 2 (for T0SZ≥24, IPA≤256MB)
             1 = start at level 1 (for T0SZ≥16, IPA≤64GB)
  VTCR.IRGN: Inner cache write-back for PTW
  VTCR.ORGN: Outer cache write-back for PTW
  VTCR.SH:   Shareability for PTW (Inner shareable for SMP)
  
For 32-bit guest (4GB IPA = 2^32):
  T0SZ = 0 → IPA = 2^32 = 4GB
  SL0 = 1 → start at L1 (4 entries covering 4×1GB)
```

---

## 5. VMID and TLB Management

### 5.1 VMID vs ASID

```
ASID (Application Space ID): 8-bit, in CONTEXTIDR[7:0]
  Identifies address space within a VM (or host)
  Tags Stage-1 TLB entries
  0 reserved for kernel (global entries)
  
VMID (Virtual Machine ID): 8-bit, in VTTBR[55:48]
  Identifies which VM owns a TLB entry
  Tags Stage-2 TLB entries
  Used to avoid TLB flush on VM context switch (like ASID for VMs)
  
Combined TLB tag: {VMID, ASID, VA} → PA
  VMID 0: host Linux (no Stage-2 for host with KVM)
  VMID 1: VM #1
  VMID 2: VM #2
  ...

TLB flush operations with VMID:
  TLBIALLH:   Invalidate all TLB entries (all VMIDs) — broadcast in SMP
  TLBIALLIS:  Inner-shareable all TLB invalidate
  TLBIALLHIS: Inner-shareable all Stage-2 (all VMIDs)
  TLBIVMALLE1IS: Invalidate all for current VMID, all ASIDs
  TLBIIPAS2:  Invalidate by IPA (Stage-2 specific)
  TLBIIPAS2IS: Inner-shareable TLB invalidate by IPA
```

### 5.2 VM Context Switch

```assembly
/* KVM: Switch from VM1 to VM2 */

/* 1. Save VM1 state (CONTEXTIDR, TTBR0, other banked regs) */
MRC  p15, 0, r0, c13, c0, 1    @ Read CONTEXTIDR (VM1's ASID)
STR  r0, [vcpu1, #VCPU_CONTEXTIDR]
MRC  p15, 0, r0, c2, c0, 0     @ Read TTBR0 (VM1's page table)
STR  r0, [vcpu1, #VCPU_TTBR0]

/* 2. Update VTTBR to point to VM2's Stage-2 tables */
/* VTTBR = (VMID << 48) | Stage2_PGD_PA */
LDR  r0, [vcpu2, #VCPU_VTTBR_LO]
LDR  r1, [vcpu2, #VCPU_VTTBR_HI]
MCRR p15, 6, r0, r1, c2         @ VTTBR = new VM2 VTTBR

ISB                               @ Ensure VTTBR change takes effect

/* 3. Restore VM2 state */
LDR  r0, [vcpu2, #VCPU_CONTEXTIDR]
MCR  p15, 0, r0, c13, c0, 1    @ Restore CONTEXTIDR (VM2's ASID)
ISB
LDR  r0, [vcpu2, #VCPU_TTBR0]
MCR  p15, 0, r0, c2, c0, 0     @ Restore TTBR0 (VM2's page tables)
ISB

/* No TLB flush needed if VMID unique per VM */
/* TLB entries tagged with VMID: VM1 entries automatically ignored */
```

---

## 6. HVC / World Switch to Hypervisor

### 6.1 HVC Instruction

```assembly
/* HVC: Hypervisor Call */
/* Similar to SVC (supervisor call) but targets PL2 */
/* Causes exception to Hyp mode if:
     - EL1 (PL1) executes HVC
     - HCR.HVC bit not set to suppress
*/

/* Hypercall convention (KVM/ARM PSCI) */
MOV  r0, #PSCI_FUNCTION_ID    @ Function: CPU_ON, CPU_OFF, etc.
MOV  r1, #target_cpu
MOV  r2, #entry_pa
MOV  r3, #context_id
HVC  #0                        @ Call hypervisor

/* Hypervisor vector table (HVBAR) */
HVBAR entry for HVC:
  → ARM_EXCEPTION_HVC handler in arch/arm/kvm/interrupts_head.S

/* HSR (Hyp Syndrome Register) after HVC: */
HSR.EC = 0x12 (HVC instruction)
HSR.ISS = immediate16 value from HVC instruction
```

### 6.2 VM Exit and Entry Flow

```
VM Exit (guest → hypervisor):

1. Exception in guest (PL1 or PL0):
   - Data abort (page fault, MMIO)
   - IRQ/FIQ (from physical device — routed to Hyp via HCR.IMO/FMO)
   - HVC instruction (hypercall)
   - SMC instruction (if HCR.TSC=1: trap to Hyp)
   - WFI/WFE (if HCR.TWI/TWE=1: trap to Hyp)
   - CP15 access (if HSTR bits set)
   
2. Hardware: saves guest PC, CPSR to ELR_hyp, SPSR_hyp
3. Branches to HVBAR + appropriate vector offset
4. Hypervisor handler runs in PL2:
   - Reads HSR for fault cause
   - Reads HDFAR/HIFAR/HPFAR for fault address
   
5. Hypervisor handles:
   - MMIO emulation
   - IRQ injection
   - Shadow device emulation
   - Page fault handling (Stage-2 install)
   
VM Entry (hypervisor → guest):

1. Restore guest register state (GPRs, CPSR, PC, CP15)
2. Set VTTBR to guest's Stage-2 table
3. Execute ERET instruction → returns to guest at ELR_hyp
4. Hardware restores SPSR_hyp → guest CPSR
5. Guest continues from interrupted point

Cost: ~1000-2000 cycles per VM exit/entry (register save/restore + pipeline flush)
```

---

## 7. KVM/ARM Architecture

### 7.1 KVM/ARM Software Stack

```
┌─────────────────────────────────────────────────────────────────────┐
│  QEMU / Virtual Machine Monitor (VMM)                               │
│    - Device emulation (virtio, e1000, etc.)                         │
│    - MMIO range registration                                        │
│    - CPU state serialization                                        │
└────────────────────────────────────────────────────────────────────┘
              │
              │ ioctl(fd, KVM_RUN, ...)
              ▼
┌─────────────────────────────────────────────────────────────────────┐
│  KVM module (virt/kvm/ + arch/arm/kvm/)                            │
│                                                                     │
│  kvm_vcpu_run():                                                    │
│    1. kvm_arm_vcpu_enter_exit(): enter guest (world switch)        │
│    2. Handle VM exit                                                │
│    3. If mmio_needed: return to QEMU for device emulation          │
│    4. Else: inject IRQ/handle internally, re-enter guest           │
│                                                                     │
│  Stage-2 MMU: kvm_mmu_map_page(), kvm_handle_guest_abort()        │
│  GIC emulation: kvm_vgic_inject_irq()                              │
│  Timer emulation: kvm_timer_update_run()                           │
│  PSCI emulation: kvm_psci_call()                                   │
└─────────────────────────────────────────────────────────────────────┘
              │
              │ (Hyp mode, PL2)
              ▼
┌─────────────────────────────────────────────────────────────────────┐
│  KVM Hypervisor (arch/arm/kvm/hyp/)                                │
│    - __kvm_vcpu_run_nvhe(): final world switch                     │
│    - Save/restore host+guest CP15 registers                        │
│    - Configure VTTBR, HCR                                          │
│    - ERET to guest                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.2 KVM Key Data Structures

```c
/* arch/arm/kvm/arm.c, arch/arm/include/asm/kvm_host.h */

struct kvm_vcpu_arch {
    struct kvm_cpu_context ctxt;       /* Guest CPU register state */
    
    /* Stage-2 MMU */
    unsigned long hcr;                  /* Guest HCR value */
    struct kvm_s2_mmu mmu;             /* Stage-2 MMU state */
    
    /* CP15 state (saved on VM exit, restored on VM entry) */
    u32 cp15[NR_CP15_REGS];
    
    /* VGIC state */
    struct vgic_cpu vgic_cpu;
    
    /* Timer */
    struct arch_timer_cpu timer_cpu;
    
    /* Fault information */
    struct kvm_vcpu_fault_info fault;  /* HSR, HDFAR, HIFAR, HPFAR */
};

struct kvm_s2_mmu {
    struct kvm *kvm;
    pgd_t *pgd;                        /* Stage-2 L1 page table (IPA space) */
    phys_addr_t pgd_phys;              /* Physical address of pgd */
    u64 vmid_gen;                      /* VMID generation counter */
    u32 vmid;                          /* Current VMID */
};
```

---

## 8. KVM MMU — Stage-2 Page Tables

### 8.1 Stage-2 Page Table Management

```c
/* arch/arm/kvm/mmu.c */

/*
 * kvm_alloc_stage2_pgd(): Allocate VM's Stage-2 L1 table
 * Called when VM is created
 */
int kvm_alloc_stage2_pgd(struct kvm *kvm)
{
    pgd_t *pgd;

    if (kvm->arch.mmu.pgd != NULL)
        return -EINVAL;

    pgd = alloc_pages_exact(S2_PGD_SIZE, GFP_KERNEL | __GFP_ZERO);
    if (!pgd)
        return -ENOMEM;

    /* Set VTTBR: VMID (8-bit) in bits [55:48] + physical base */
    kvm->arch.mmu.pgd = pgd;
    kvm->arch.mmu.pgd_phys = virt_to_phys(pgd);

    return 0;
}

/*
 * kvm_mmu_map_page(): Install a Stage-2 mapping
 *   IPA range [base_ipa, base_ipa + size) → PA pfn
 */
static int stage2_map_walk(struct kvm *kvm, phys_addr_t start,
                            phys_addr_t size, phys_addr_t pfn, kvm_pgtable_prot_t prot)
{
    struct kvm_pgtable *pgt = &kvm->arch.mmu.pgt;
    return kvm_pgtable_stage2_map(pgt, start, size, pfn << PAGE_SHIFT, prot, NULL);
}

/*
 * kvm_pgtable_stage2_map() ultimately:
 *   1. Walks Stage-2 tables (allocating L2/L3 tables as needed)
 *   2. Installs LPAE descriptors: PA + S2AP (read/write) + MemAttr
 *   3. DSB + TLBI to flush Stage-2 TLB
 */
```

### 8.2 Demand Paging for Stage-2 (Most Important KVM Concept)

```
Stage-2 page tables are populated on DEMAND (like CPU page faults):

1. VM starts: Stage-2 tables EMPTY (all IPAs unmapped)
2. Guest touches any memory → Stage-2 fault → Hyp mode
3. KVM handles fault: maps IPA → PA in Stage-2 tables
4. Guest continues: next access hits in TLB (no fault)

Why demand paging?
  - VM may have 4GB IPA space, but only uses 512MB
  - Pre-populating all 4GB would waste memory
  - Same benefit as CPU lazy page allocation

Stage-2 fault flow:
  Guest load/store → Stage-2 TLB miss → Stage-2 PTW → fault (IPA not in tables)
  → Vector to Hyp mode (HVBAR)
  → Read HPFAR (faulting IPA)
  → kvm_handle_guest_abort(vcpu, run)
  → Determine if MMIO or RAM (check against VM memory regions)
  → If RAM: install Stage-2 mapping
  → If MMIO: return to user-space (QEMU) for device emulation
```

---

## 9. KVM Guest Abort Handling

### 9.1 Data Abort Handler

```c
/* arch/arm/kvm/mmio.c, arch/arm/kvm/mmu.c */

int kvm_handle_guest_abort(struct kvm_vcpu *vcpu, struct kvm_run *run)
{
    unsigned long fault_status;
    phys_addr_t fault_ipa;
    struct kvm_memory_slot *memslot;
    bool is_iabt, write_fault, writable;

    fault_status = kvm_vcpu_get_fault_info(vcpu);
    fault_ipa    = kvm_vcpu_get_fault_ipa(vcpu);  /* From HPFAR */

    /* Is this a Stage-2 fault? */
    if (!kvm_is_stage2_fault(fault_status))
        return 1; /* Let host kernel handle */

    /* Is this MMIO? (IPA in device range, not in memslot) */
    memslot = gfn_to_memslot(vcpu->kvm, gpa_to_gfn(fault_ipa));
    if (!memslot) {
        /* MMIO region: emulate in user-space (QEMU) */
        return io_mem_abort(vcpu, run, fault_ipa);
    }

    /* Regular RAM: install Stage-2 mapping */
    return user_mem_abort(vcpu, run, memslot, fault_ipa, fault_status);
}

static int user_mem_abort(struct kvm_vcpu *vcpu, struct kvm_run *run,
                           struct kvm_memory_slot *memslot,
                           phys_addr_t fault_ipa, unsigned long fault_status)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    pfn_t pfn;
    kvm_pgtable_prot_t prot = KVM_PGTABLE_PROT_R | KVM_PGTABLE_PROT_W;

    /* Get the host pfn for this IPA (resolve host VA → PA) */
    vma = find_vma(mm, hva);           /* host virtual address */
    pfn = follow_pfn(vma, hva, ...);

    /* Install Stage-2 mapping: IPA → PA */
    ret = kvm_pgtable_stage2_map(&vcpu->kvm->arch.mmu.pgt,
                                   fault_ipa, PAGE_SIZE,
                                   pfn << PAGE_SHIFT, prot, NULL);

    /* TLB invalidation for this IPA: */
    /* TLBIIPAS2IS: Inner-shareable TLB invalidate by IPA */
    kvm_tlb_flush_vmid_ipa(vcpu->kvm, fault_ipa, 0);

    return 0;
}
```

### 9.2 MMIO Emulation

```c
/* MMIO emulation flow:
 *
 * Guest: STR r0, [r1]  where r1=0x08000000 (device MMIO IPA)
 * Fault: Stage-2 miss → HSR.ISV=1 (instruction syndrome valid)
 * HSR.WnR=1 (write), HSR.SRT=0 (r0 = source), HSR.SAS=2 (32-bit)
 *
 * KVM reads from guest register file: run->mmio.data = vcpu_reg(vcpu, 0)
 * KVM sets: run->exit_reason = KVM_EXIT_MMIO
 *           run->mmio.phys_addr = 0x08000000
 *           run->mmio.is_write = 1
 *           run->mmio.len = 4
 *           run->mmio.data = value of r0
 *
 * KVM returns to QEMU → QEMU calls back into KVM_RUN after handling device
 *
 * Reads: QEMU fills run->mmio.data with device register value
 *        KVM injects value into guest register: vcpu_reg(vcpu, SRT) = run->mmio.data
 */
```

---

## 10. SMMU Stage-2 — Device Passthrough to VMs

### 10.1 Stage-2 for DMA (SMMU Virtualization)

```
Problem with device passthrough without Stage-2 SMMU:
  VM gets direct access to physical device (e.g., GPU passthrough)
  Device DMA uses IOVA from VM's IOMMU domain
  Without Stage-2: device can DMA to any PA → security hole

Solution: SMMU Stage-2
  SMMU supports same two-stage translation as CPU:
  Stage-1 (guest IOMMU driver): IOVA → Guest-PA (IPA)
  Stage-2 (hypervisor): IPA → Host-PA
  
  Result: Device DMA restricted to VM's physical memory
          VM cannot DMA outside its assigned Stage-2 range
```

### 10.2 SMMU Stage-2 Setup in KVM

```c
/* arch/arm/kvm/arm.c + drivers/iommu/arm-smmu.c */

/*
 * When a device is assigned to a VM:
 * 1. Create SMMU domain for the device
 * 2. Configure Stage-2 in SMMU context bank
 *    (uses same VTTBR as CPU Stage-2 — VMID must match)
 * 3. Device DMA now goes through: IOVA → IPA → PA
 *
 * ARM SMMU context bank Stage-2 configuration:
 *   CBAR.TYPE = 0b01 (Stage-1 fault + Stage-2)
 *   or  CBAR.TYPE = 0b10 (Stage-2 only, passthrough Stage-1)
 *
 *   VTTBR = same as CPU VTTBR for this VM's VMID
 *   VTCR  = same as CPU VTCR
 *
 * This way: CPU Stage-2 table = SMMU Stage-2 table
 * No separate page tables! Unified memory protection.
 */

static int kvm_arm_setup_smmu_s2(struct kvm *kvm, struct device *dev)
{
    struct iommu_domain *s2_domain;
    
    /* Create an IOMMU domain that uses VM's Stage-2 tables */
    s2_domain = iommu_domain_alloc(&platform_bus_type);
    
    /* Tell SMMU driver to use VM's VTTBR/VMID */
    iommu_domain_set_attr(s2_domain, DOMAIN_ATTR_NESTING,
                           &kvm->arch.mmu);

    /* Attach device to this domain */
    iommu_attach_device(s2_domain, dev);
    
    return 0;
}
```

---

## 11. Protected KVM (pKVM) — Google's Hypervisor

### 11.1 pKVM Motivation

```
Standard KVM/ARM security model:
  Host Linux kernel in PL1 (ring 0)
  KVM module runs partially in PL2
  
  Problem: Host Linux kernel has full access to ALL physical memory.
    A kernel bug or exploit → host can read/write any VM's memory
    A compromised host → all VMs compromised (multi-tenant cloud issue)
    
pKVM (Protected KVM) — Android 13+, mainline Linux 5.20+:
  Hypervisor (small trusted base) lives permanently in PL2
  Host Linux kernel demoted to EL1 (same as guests)
  Hypervisor enforces isolation between host kernel and VMs
  
  Threat model:
    ✓ Compromised host kernel CANNOT read VM memory
    ✓ VM CANNOT read host kernel memory
    ✓ VM CANNOT read other VMs' memory
    ✓ Only hypervisor (small code) is in Trusted Computing Base (TCB)
```

### 11.2 pKVM Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│  PL2: pKVM Hypervisor (small, ~10K lines, verified)              │
│                                                                    │
│  - Controls ALL Stage-2 tables (host + VMs)                      │
│  - Host kernel gets Stage-2 table: read-only for VM memory       │
│  - VM gets Stage-2 table: isolated from host/other VMs           │
│  - Memory donation: host → VM via hypervisor calls               │
│  - SMMU: hypervisor programs SMMU Stage-2                        │
└───────────────────────────────────────────────────────────────────┘
        │ hvc #KVM_HOST_SMCCC_FUNC           │ ERET
        ▼                                     ▲
┌────────────────────┐               ┌────────────────────┐
│   Host Kernel      │               │   Guest VM         │
│   (PL1, EL1)       │               │   (PL1, EL1)       │
│                    │               │                    │
│   KVM module:      │               │   Guest kernel     │
│   QEMU, virtio     │               │   Guest apps       │
└────────────────────┘               └────────────────────┘

Memory isolation:
  Host can see:   Host memory + shared memory (explicitly donated)
  Guest can see:  Guest memory + shared memory (explicitly donated)
  Neither can see the other's private memory
```

### 11.3 pKVM Memory Donation

```c
/* arch/arm64/kvm/hyp/nvhe/mem_protect.c (conceptual ARM32 equivalent) */

/*
 * When host wants to give memory to VM:
 * 1. Host calls: kvm_call_hyp(HYP_DONATE_PAGE, pa, vm_id)
 * 2. Hypervisor (PL2):
 *    a. Remove page from host's Stage-2 table (host can no longer access)
 *    b. Add page to guest's Stage-2 table (guest can access)
 *    c. DSB + TLBI to flush both Stage-2 tables
 *
 * When VM releases memory back to host:
 * 1. VM calls: hvc #HYPERVISOR_PAGE_RECLAIM, ipa
 * 2. Hypervisor:
 *    a. Remove page from guest's Stage-2 table
 *    b. Scrub page contents (zeroise: prevent info leak)
 *    c. Add page back to host's Stage-2 table
 *
 * Shared memory (virtio buffers):
 * 1. Host maps page as SHARED (read-write in both Stage-2 tables)
 * 2. Guest maps page as SHARED
 * 3. virtio driver uses this shared page for data transfer
 */
```

---

## 12. Performance and Trade-offs

### 12.1 VM Exit Frequency

```
Common VM exit causes and frequencies (typical workload):

Cause                   Frequency   Latency    Optimization
──────────────────────────────────────────────────────────────
MMIO (device access)    10K/sec     1-5 µs     Virtio (fewer exits)
Timer interrupt         100-1000/s  1 µs       In-kernel timer emulation
IPI (inter-processor)   1000/s      1 µs       IPI batching
TLB maintenance         100/s       1 µs       No exit needed (Stage-2)
PSCI CPU_ON/OFF         rare        5-10 µs    Fast path in KVM
Page fault (Stage-2)    rare        10-50 µs   Huge pages (1MB blocks)
```

### 12.2 Two-Stage Translation Overhead

```
Without virtualization: VA → PA  (1 page table walk)
With virtualization: VA → IPA → PA  (up to 2×5 = 10 table lookups worst case)

Worst-case two-stage walk (no TLBs, 3-level S1 + 3-level S2):
  Stage-1 L1 fetch: S2 walk needed → 4 PA reads from S2 tables
  Stage-1 L2 fetch: S2 walk needed → 4 PA reads from S2 tables  
  Stage-1 L3 fetch: S2 walk needed → 4 PA reads from S2 tables
  Final PA:         S2 walk needed → 4 PA reads from S2 tables
  Total: up to 24 memory reads (vs 4 for Stage-1 only, 2 for no virtualization)
  
Real-world (with TLB):
  TLB hit: 0 additional cycles (combined {VMID,ASID,VA}→PA cached)
  TLB miss with S2 TLB warm: ~10-15 additional cycles
  Full double-walk: ~100-200 additional cycles (rare)

Mitigation:
  1. Large pages: 1MB block in Stage-2 reduces PTW levels
  2. VMID-tagged TLB: avoid TLB flush on VM switch (like ASID)
  3. Hardware page table walker caches S2 entries efficiently
```

### 12.3 Shadow Page Tables vs Hardware Stage-2

```
Early ARM virtualization (before ARM VE): Shadow Page Tables
  Hypervisor maintains "shadow" page tables that map VA → PA directly
  Guest updates its page tables → trap to hypervisor → update shadows
  
  Problems:
    - Trap on every guest page table update (VERY expensive)
    - Memory: 2× page tables (guest + shadow)
    - Code complexity: hypervisor must understand guest OS PT format
    
Hardware Stage-2 (ARM Virtualization Extensions):
  Guest manages Stage-1 freely (no trapping)
  Hypervisor manages Stage-2 (IPA → PA)
  Hardware does both walks transparently
  
  Advantages:
    - No trap on guest page table updates
    - Clean separation: guest controls S1, hyp controls S2
    - VMID eliminates TLB flush on VM switch
    
  Tradeoff:
    - Two-stage walk overhead (mitigated by TLB)
    - Requires hardware support (ARM VE mandatory for KVM/ARM)
```

---

## Summary

| Component | Register | Purpose |
|-----------|----------|---------|
| Hyp mode | PL2 | Hypervisor privilege level |
| HVC | Instruction | Hypercall (PL1 → PL2) |
| HCR | CP15 c1,1,0 | Configure trapping/virtualization |
| VTTBR | 64-bit | Stage-2 PGD base + VMID |
| VTCR | CP15 c2,1,2 | Stage-2 translation control |
| HVBAR | CP15 c12,0,0 | Hyp vector base address |
| HSR | CP15 c5,4,2 | Hyp syndrome (fault cause) |
| HPFAR | CP15 c6,4,4 | IPA of faulting Stage-2 access |
| VMID | VTTBR[55:48] | 8-bit VM identifier (TLB tag) |

---

**Cross-References:**
- Doc 01: ARM32 page table formats (Stage-1 still uses short/long descriptor)
- Doc 03: Linux MM internals for host kernel running under KVM
- Doc 04: TLB management extended with VMID for two-stage TLB tagging
- Doc 06: TrustZone + Hyp mode coexistence (Secure Monitor vs Hyp mode)
- Doc 07: SMMU Stage-2 for device passthrough to VMs
- Doc 08: Memory barriers critical for Stage-2 table updates (DSB + TLBI)

---
**End of Document 9**
