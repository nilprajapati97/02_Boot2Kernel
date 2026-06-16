# KVM ARM64: Kernel-based Virtual Machine on AArch64

**Category**: Virtualization Memory and Stage 2 Translation  
**Platform**: ARM64 (AArch64)

---

## 1. KVM/ARM64 Architecture

```
KVM on ARM64: Linux Kernel Virtual Machine
  Hypervisor type: Type-2 (runs inside Linux host kernel, EL2)
  Alternative to: Xen (type-1 on ARM), Hyper-V on ARM, pKVM

ARM64 KVM modes:

1. Non-VHE mode (traditional, older ARM64):
   EL2: KVM lowvisor (small EL2 code: world-switch, Stage 2 setup)
   EL1: Host Linux kernel (full kernel at EL1)
   EL0: Host userspace + QEMU
   VM entry: EL1→EL2 transition + switch to guest TTBR + enable S2 + EL2→EL1(guest)
   VM exit: guest EL1→EL2 + save state + EL2→EL1(host) + deliver event
   Problem: world switch overhead = two EL2 transitions per entry/exit

2. VHE mode (Virtualization Host Extensions, ARM8.1+, most modern ARM64):
   EL2+Host mode: host Linux kernel runs at EL2 directly!
   EL0: applications and QEMU (still at EL0)
   Guest: guest EL1 = EL1 (with Stage 2 enabled for guest)
   Benefits:
     No EL2/EL1 boundary for host kernel (kernel at EL2)
     VM entry: EL0 (QEMU) → EL2 (host kernel KVM code) → guest EL1
     Fewer transitions, host kernel can access EL2 registers
     HCR_EL2.E2H=1 + HCR_EL2.TGE=1 → EL2 system regs aliased to EL1 names
   All modern ARM64 server use VHE (Neoverse N1/N2, Apple M-series)

KVM components:
  arch/arm64/kvm/: ARM64 KVM code
    arm.c: KVM init, VCPU setup
    mmu.c: Stage 2 page table management (core file for memory)
    hyp/: EL2 code (hypervisor entry/exit, world switch)
    vgic/: virtual GIC (interrupt controller emulation)
    pmu.c: PMU emulation
  virt/kvm/: architecture-agnostic KVM code
  tools/kvm/: KVM test tools
```

---

## 2. Stage 2 Page Table Management in KVM

```c
/* arch/arm64/kvm/mmu.c - Stage 2 page table management */

struct kvm_s2_mmu {
    struct kvm_vmid vmid;         // VMID for this address space
    pgd_t *pgd;                   // Stage 2 page table base
    phys_addr_t pgd_phys;         // physical address of pgd
    int pgd_level;                // number of levels (2-4)
};

struct kvm_arch {
    struct kvm_s2_mmu mmu;        // EL1&0 Stage 2 translation
    u64 vtcr;                     // VTCR_EL2 value for this VM
    ...
};

Stage 2 page table creation:
  kvm_mmu_create():
    kvm_pgtable_stage2_init():
      Allocate pgd (Level 0 table for Stage 2)
      Set kvm->arch.vtcr = compute_vtcr()
      VTCR_EL2 fields:
        PS = ID_AA64MMFR0_EL1.PARange (from hardware)
        T0SZ = 64 - IPA_SIZE (typically 40 bits → T0SZ=24)
        TG0 = 4KB granule (0b00)
        SH0 = Inner Shareable (0b11)
        IRGN0 = WB RA WA (0b01)
        ORGN0 = WB RA WA (0b01)
        SL0: start level based on T0SZ

  kvm_pgtable_stage2_map(pgt, ipa, size, pa, prot, mc):
    Walks Stage 2 page tables (like cpu_install_idmap but for S2)
    Creates: IPA → PA mapping
    Called when:
      Guest page fault (kvm_handle_guest_abort)
      Backing memory pre-mapped (huge pages optimization)
    
    Granularity: 4KB, 2MB (block), or 1GB (block)
    Huge page decision: if guest uses huge page AND physical memory contiguous:
      Use 2MB Stage 2 block entry → saves IOTLB entries, better TLB utilization

  kvm_pgtable_stage2_unmap(pgt, ipa, size):
    Remove Stage 2 mapping
    Used when: VM memory unmapped (munmap of QEMU guest RAM)
    
  kvm_pgtable_stage2_wrprotect(pgt, ipa, size):
    Mark Stage 2 entry as read-only
    Used for: dirty page tracking (live migration)
    Write fault → stage2_is_exec(entry) for COW detection

Stage 2 fault types:
  FSC (Fault Status Code) in ESR_EL2:
    0b000000: Address size fault, L0
    0b000100: Translation fault, L0
    0b000101: Translation fault, L1
    0b000110: Translation fault, L2
    0b000111: Translation fault, L3
    0b001101: Permission fault, L1
    0b001110: Permission fault, L2
    0b001111: Permission fault, L3
    0b100000: Synchronous external abort
```

---

## 3. Guest Page Fault Handling

```c
/* arch/arm64/kvm/mmu.c */

/* kvm_handle_guest_abort: handle Stage 2 fault */
int kvm_handle_guest_abort(struct kvm_vcpu *vcpu)
{
    /* 1. Extract fault information from ESR_EL2 and FAR_EL2 */
    hsr = vcpu->arch.fault.esr_el2;
    fault_ipa = kvm_vcpu_get_fault_ipa(vcpu); // IPA from HPFAR_EL2
    fault_status = FSC(hsr);
    
    /* 2. Find QEMU memory slot for this IPA */
    memslot = gfn_to_memslot(kvm, gpa_to_gfn(fault_ipa));
    if (!memslot)
        return -EFAULT; // no backing memory → inject guest data abort
    
    /* 3. Get host physical address */
    hva = gfn_to_hva(kvm, gpa_to_gfn(fault_ipa));
    // hva = QEMU virtual address backing this guest PA
    
    /* 4. Get struct page for the HVA */
    // If not yet faulted in: handle_mm_fault(hva) will allocate page
    ret = kvm_map_hva_to_ipa(vcpu, fault_ipa, hva);
    
    /* 5. Create Stage 2 mapping */
    // kvm_pgtable_stage2_map(ipa, pa, PAGE_SIZE, prot)
    // Now guest can access this page without faulting
    
    return 0; // success, resume guest
}

HPFAR_EL2 (Hypervisor IPA Fault Address Register):
  Contains: faulting IPA (page-aligned)
  Read as: HPFAR_EL2.FIPA[47:4] × 256 + FAR_EL2[11:0]
  ARM64: HPFAR_EL2 populated automatically on Stage 2 fault
  (vs x86: VMM must decode the page walk to find guest PA — expensive!)

Stage 2 fault optimization: large page coalescing
  Initially: guest accesses many 4KB pages → many 4KB Stage 2 entries
  kcompactd/khugepaged equivalent for S2:
    kvm_mmu_try_split_huge_page(): split 2MB→4KB if needed
    kvm_mmu_huge_page_coalescible(): coalesce adjacent 4KB → 2MB
    Reduces TLB entries → better VM performance
  
  ARM64 KVM: transparent 2MB/1GB Stage 2 huge pages
    Condition: host has 2MB physically contiguous pages
    AND: guest mapping aligns to 2MB boundary
    Result: kvm_pgtable_stage2_map() creates 2MB block entry
    TLB: one entry covers 2MB → 512× fewer S2 TLB entries
```

---

## 4. VHE (Virtualization Host Extensions)

```
VHE: ARM8.1 feature that allows host kernel to run at EL2

Problem with non-VHE:
  Two separate kernels:
    EL1: full Linux kernel
    EL2: small KVM lowvisor (limited, must be maintained separately)
  Context switch: EL1 Linux → EL2 KVM → guest EL1 → exit back through EL2 → EL1
  Code duplication: EL2 must reimplement some EL1 functionality

VHE solution (HCR_EL2.E2H = 1):
  Same kernel binary runs at EL2 (host) and EL1 (guest)
  System register aliasing: when E2H=1:
    Access to EL1 registers (e.g., TTBR0_EL1): actually read/write TTBR0_EL2
    E2H bit: hardware transparently remaps sysreg accesses
  TGE bit (HCR_EL2.TGE=1): trap all EL0 exceptions to EL2 (host EL0 = QEMU)
  
  VHE register mapping (E2H=1):
    Alias:                 Actual register:
    TTBR0_EL1            = TTBR0_EL2
    TTBR1_EL1            = TTBR1_EL2
    TCR_EL1              = TCR_EL2 (extended format)
    SCTLR_EL1            = SCTLR_EL2
    SPSR_EL1             = SPSR_EL2
    ELR_EL1              = ELR_EL2
    SP_EL0               = SP_EL0 (no change)
    MAIR_EL1             = MAIR_EL2
    
  VHE context switch:
    QEMU (EL0) triggers KVM hypercall (HVC) or fault
    → EL2 (host kernel KVM code) handles it
    → Switch VTTBR_EL2 to guest's S2 table
    → Clear TGE bit: guest EL0 exceptions go to guest EL1
    → ERET to guest EL1
    Guest exit:
    → HVC / abort / interrupt → EL2 (KVM code)
    → Save guest state, restore host state
    → Set TGE=1: back to host mode
    → Return to QEMU (EL0 via EL2)

KVM with VHE: arch/arm64/kvm/hyp/nvhe/ vs hyp/vhe/
  vhe/ : VHE-optimized code paths
  nvhe/: Non-VHE code paths (older ARM64 or intentionally non-VHE)
  
  Linux selects at runtime: is_kernel_in_hyp_mode() == true → VHE active
```

---

## 5. Interview Questions & Answers

**Q1: How does ARM64 Stage 2 translation protect VMs from each other?**

Each VM has its own Stage 2 page table (pointed to by `VTTBR_EL2`, which includes the VM's VMID). The Stage 2 table maps IPA (what the guest thinks is physical memory) to actual host physical addresses (PA).

The hypervisor (KVM) controls the Stage 2 table exclusively (requires EL2 privilege). When KVM creates a VM with 4GB of RAM, it:
1. Calls `mmap()` in QEMU to allocate 4GB of host virtual memory (backed by host physical pages)
2. Maps those host pages into the Stage 2 table: IPA 0x00000000..0xFFFFFFFF → host PA

VM-A's Stage 2 table maps only VM-A's physical pages. VM-B's Stage 2 table maps only VM-B's pages. There is no overlap. If VM-A's guest OS tries to access IPA 0x100000000 (outside its RAM):
- Stage 2 lookup: no valid entry at that IPA → Stage 2 translation fault
- Fault taken to EL2 (KVM): `kvm_handle_guest_abort()`
- KVM checks: is this IPA mapped in VM-A's memslots? NO → inject data abort to guest
- Guest sees: a data abort (like accessing unmapped memory)

VM-A's guest OS cannot access VM-B's memory because VM-B's memory is not mapped in VM-A's Stage 2 table. The hypervisor is the sole keeper of Stage 2 tables, and it never maps VM-B's pages into VM-A's domain.

**Q2: What is VHE and why is it important for ARM64 server performance?**

VHE (Virtualization Host Extensions, ARMv8.1) allows the host Linux kernel to run natively at EL2 instead of EL1. This eliminates the traditional hypervisor split between a large host kernel at EL1 and a small "lowvisor" at EL2.

Performance benefits:
1. **Fewer exception level transitions**: Without VHE, VM entry/exit requires EL1→EL2→EL1 (guest) transitions — each transition flushes some CPU state, requires register save/restore. With VHE: host runs AT EL2, so no EL1↔EL2 boundary for host operations.
2. **No code duplication**: Without VHE, the EL2 lowvisor had to reimplement some Linux kernel functionality (memory allocation, locks). With VHE: same kernel code runs at EL2.
3. **Better cache performance**: host kernel + hypervisor in same exception level = better instruction cache utilization.
4. **Simpler pointer authentication (FEAT_PAuth)**: With VHE, the kernel's PAuth setup applies uniformly.

On AWS Graviton 3, Ampere Altra, and Apple M-series (all Neoverse N1-class or equivalent): KVM uses VHE mode. VM entry/exit overhead is typically < 1µs on these platforms. This allows running thousands of short-lived containers as lightweight VMs (AWS Firecracker) with minimal overhead.

---

## 6. Quick Reference

| KVM Concept | ARM64 Implementation |
|---|---|
| Hypervisor level | EL2 (non-VHE) or EL2+Host (VHE) |
| Stage 2 table base | VTTBR_EL2 (PA + VMID) |
| Stage 2 control | VTCR_EL2 |
| Stage 2 enable | HCR_EL2.VM=1 |
| Guest A64 mode | HCR_EL2.RW=1 |
| VM Entry | eret to guest EL1 |
| VM Exit | hvc, smc, data/inst abort, IRQ/FIQ |
| VMID (8-bit) | 256 VMs in TLB simultaneously |
| VMID (16-bit) | 65536 VMs (VTCR_EL2.VS=1) |

| Stage 2 Fault | Cause | KVM Action |
|---|---|---|
| Translation fault L3 | Guest accesses new page | Allocate host page, map S2 |
| Translation fault L1/L2 | Large region not mapped | Map with huge page |
| Permission fault | Write to read-only (dirty tracking) | Mark page dirty, re-map RW |
| Access flag fault | AF not set | Set AF in S2 PTE (if no HAFDBS) |
