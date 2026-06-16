# nVHE KVM Stub — EL2 Without Host Extensions

## nVHE Architecture

When VHE is NOT available (ARMv8.0 hardware) or VHE is disabled:
```
Non-VHE Kernel Layout:

EL3 (Secure Monitor / TrustZone)
 │
 └─ EL2 (KVM Hypervisor — small stub only)
      │  VBAR_EL2 = __kvm_hyp_vector
      │  All exception routing: HCR_EL2.IMO/FMO/AMO = 1
      │
      └─ EL1 (Linux Host Kernel — normal operation)
           │
           └─ EL0 (User Processes)
```

The KVM "stub" at EL2 is minimal: it only handles:
1. Switching between host and guest EL1 contexts
2. Routing exceptions appropriately
3. Maintaining isolation between host and guest

---

## `finalise_el2_nvhe` Implementation

```asm
// arch/arm64/kernel/head.S (simplified):
SYM_FUNC_START(finalise_el2_nvhe)
    /*
     * nVHE: Kernel will run at EL1. Configure EL2 as KVM-accessible.
     * After this, eret will take us to EL1h mode.
     */
    
    // Set HCR_EL2 for nVHE host mode:
    mov     x0, #HCR_RW            // only need 64-bit EL1
    msr     hcr_el2, x0            // minimal EL2 config until KVM loads
    
    // Set SCTLR_EL2 to sane defaults:
    mov     x0, SCTLR_ELx_FLAGS
    msr     sctlr_el2, x0
    
    // Configure SPSR_EL2 for eret to EL1h:
    mov     x0, #(PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT | PSR_MODE_EL1h)
    msr     spsr_el2, x0
    
    // Timer configuration (same as VHE):
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    
    // Optional: set up EL2 exception vectors NOW:
    // (KVM replaces these when kvm_arch_init runs)
    adr     x0, vectors             // use Linux EL2 vector stub
    msr     vbar_el2, x0
    
    ret
SYM_FUNC_END(finalise_el2_nvhe)
```

After `finalise_el2_nvhe`:
- CPU is still at EL2 (function called from EL2)
- `eret` in the call chain will drop to EL1h
- Linux continues running at EL1

---

## KVM nVHE World Switch

When KVM launches a VM in nVHE mode, it performs an explicit EL1 context switch:

```
Host (EL1)                    KVM (EL2)                  Guest (EL1)
─────────────────────────────────────────────────────────────────────
kvm_vcpu_run()
    hvc #0              →   __kvm_vcpu_run_nvhe
                            Save host EL1 state:
                              SP_EL1, ELR_EL1,
                              SPSR_EL1, SCTLR_EL1,
                              TTBR0_EL1, TTBR1_EL1,
                              ...30+ registers
                            Load guest EL1 state
                            eret (to EL1 = guest)
                                                    →   [VM execution]
                                                        [VM triggers trap]
                                                    ←   exception to EL2
                            Save guest EL1 state
                            Restore host EL1 state
                            eret (to EL1 = host)
    ←   return from hvc
Handle exit reason
(I/O emulation, etc.)
```

This full context switch is what VHE eliminates by running the host kernel
at EL2 directly. In nVHE mode, every VM entry/exit requires ~30 register
saves plus ~30 register restores = ~1800 cycles just for register I/O.

---

## nVHE Protected Mode (pKVM)

Recent Linux kernels support "Protected nVHE" mode (pKVM):
```c
// arch/arm64/kvm/hyp/nvhe/pkvm.c
// pKVM: EL2 hypervisor with NO trust in EL1 Linux kernel
// Used for: confidential VMs, Android Protected Virtual Machines

// Structure:
EL2: pKVM hypervisor (minimal, formally verified subset)
     → Can protect VMs from the HOST Linux kernel (EL1)
     → Even if Linux is compromised, VMs remain isolated
EL1: Linux kernel (untrusted from EL2 perspective)
EL1 (guest): Protected VM (confidential)
```

`finalise_el2_nvhe` sets up the foundation for pKVM by establishing EL2
as a proper, isolated execution environment separate from EL1 Linux.

---

## nVHE: Which Registers Are Saved

During nVHE world switch, KVM saves/restores these EL1 registers:
```c
// arch/arm64/kvm/hyp/include/hyp/sysreg-sr.h (struct kvm_cpu_context):
struct kvm_cpu_context {
    struct user_pt_regs regs;    // x0-x30, sp, pc, pstate
    u64 spsr_abt;
    u64 spsr_und;
    u64 spsr_irq;
    u64 spsr_fiq;
    struct user_fpsimd_state fp_regs;  // FP/SIMD state
    
    // System registers:
    u64 sp_el1;      // stack pointer
    u64 elr_el1;     // exception link register
    u64 spsr_el1;    // saved pstate
    u64 sctlr_el1;   // system control
    u64 cpacr_el1;   // CP access control
    u64 ttbr0_el1;   // translation table base 0
    u64 ttbr1_el1;   // translation table base 1
    u64 tcr_el1;     // translation control
    u64 esr_el1;     // exception syndrome
    u64 far_el1;     // fault address
    u64 mair_el1;    // memory attribute indirection
    u64 vbar_el1;    // vector base
    u64 amair_el1;   // aux memory attribute
    u64 tpidr_el1;   // thread ID (per-cpu offset!)
    u64 tpidr_el0;   // user thread ID
    u64 tpidrro_el0; // read-only user thread ID
    // ... and more
};
```

In VHE mode, many of these are saved by normal Linux context switch
(process scheduler) — not by KVM — because the kernel lives at EL2.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.