# VHE — Virtualization Host Extensions Deep Dive

## VHE Architecture (ARMv8.1-A)

VHE was introduced in ARMv8.1-A to solve a fundamental performance problem
with running a "Type-2" hypervisor (like KVM) where the host OS runs at EL1
and the hypervisor shim runs at EL2.

```
Pre-VHE (Type-2 hypervisor problem):
┌──────────────┐
│   Guest OS   │  EL1 (guest)
├──────────────┤
│  KVM Stub    │  EL2 ← thin shim, not full kernel
├──────────────┤
│ Host Linux   │  EL1 ← host kernel here
├──────────────┤
│    EL0       │  User processes
└──────────────┘

PROBLEM: On every VM exit, the CPU must:
1. Save all guest EL1 registers
2. Restore all host EL1 registers  
3. Drop from EL2 to EL1 (host kernel)
4. Host kernel handles the exit event
→ Full register context switch: ~1000-2000 cycles overhead
```

```
With VHE (HCR_EL2.E2H = 1):
┌──────────────┐
│   Guest OS   │  EL1 (VHE redirects: guest "EL1" = actually EL1, but in VHE space)
├──────────────┤
│ Host Linux   │  EL2 ← host kernel runs AT EL2 directly! (E2H=1 makes EL1 regs alias to EL2)
├──────────────┤
│    EL0       │  User processes (TGE=1: EL0 uses host translation regime)
└──────────────┘

ADVANTAGE: On VM exit, no EL level change for host kernel
→ Register save/restore only for EL1→EL0 (guest private state)
→ ~100-300 cycles overhead per VM exit
```

---

## VHE Register Aliasing

When `HCR_EL2.E2H = 1`, the architecture creates a register aliasing mechanism:

```
Without VHE (E2H=0):         With VHE (E2H=1):
EL2 registers:               EL2 registers stay, BUT:
  SCTLR_EL2                    EL1 registers ALIAS to EL2:
  TCR_EL2                        mrs x0, sctlr_el1 → reads SCTLR_EL2
  TTBR0_EL2                      msr tcr_el1, x0   → writes TCR_EL2
  VBAR_EL2                       mrs x0, ttbr0_el1 → reads TTBR0_EL2
  ...                            ...
```

This means: Linux kernel code that uses `mrs x0, sctlr_el1` or `msr ttbr0_el1, x0`
works correctly whether running at EL1 (no VHE) or EL2 (VHE). The hardware
transparently redirects EL1 register accesses to EL2 equivalents when E2H=1.

---

## `finalise_el2_vhe` Implementation

```c
// arch/arm64/kernel/head.S
SYM_FUNC_START(finalise_el2_vhe)
    /*
     * We're running at EL2 with VHE enabled.
     * Set up SPSR_EL2 for eret to "EL2h" (host EL2 mode).
     * After this function returns, execution continues at EL2.
     */
    
    // Set SPSR_EL2 for EL2h mode (VHE host mode):
    mov     x0, #(PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT | PSR_MODE_EL2h)
    msr     spsr_el2, x0
    
    // ELR_EL2 is already set (it's the return address from the BL call)
    // When eret executes, CPU transitions to EL2h mode
    // (which is the normal operating mode for VHE Linux)
    
    // Configure CNTHCTL_EL2 for timer access:
    mov     x0, #3
    msr     cnthctl_el2, x0    // EL1 physical timer access enabled
    
    // Clear CNTVOFF_EL2:
    msr     cntvoff_el2, xzr   // no virtual timer offset for host
    
    ret
SYM_FUNC_END(finalise_el2_vhe)
```

---

## `EL2h` vs `EL1h` — PSR Mode Bits

ARM64 processor state register (SPSR/PSTATE) mode field:
```
Bits [3:0] of PSTATE.M:
    0b0000: EL0t  (EL0, using SP_EL0)
    0b0100: EL1t  (EL1, using SP_EL0 — unusual)
    0b0101: EL1h  (EL1, using SP_EL1 — normal kernel mode)
    0b1000: EL2t  (EL2, using SP_EL0 — unusual)
    0b1001: EL2h  (EL2, using SP_EL2 — VHE host mode)
    0b1100: EL3t  (EL3, using SP_EL0)
    0b1101: EL3h  (EL3, using SP_EL3)

After finalise_el2_vhe:
    PSTATE.M = 0b1001 = EL2h
    This means: CPU is at EL2, using SP_EL2 as stack pointer
    
    Linux kernel runs in EL2h mode throughout its lifetime when VHE is active
```

---

## HCR_EL2 Bit Field Reference

```
HCR_EL2 (Hypervisor Configuration Register EL2):

Bit  Name    VHE value   nVHE value  Meaning
─────────────────────────────────────────────────────────────────
31   RW      1           1           Lower ELs are AArch64
34   E2H     1           0           EL2 host mode (VHE)
27   TGE     1 (host)    0           Trap general exceptions from EL0
             0 (guest)               When TGE=1: EL0 uses host translation
5    AMO     *           1           Route SError to EL2
4    IMO     *           1           Route IRQ to EL2
3    FMO     *           1           Route FIQ to EL2

VHE full HCR_EL2 (when running host Linux):
    HCR_RW | HCR_E2H | HCR_TGE | HCR_ATA

VHE HCR_EL2 (when running VM guest):
    HCR_RW | HCR_E2H (TGE=0, AMO/IMO/FMO depend on VM config)

nVHE HCR_EL2 (after finalise_el2_nvhe):
    HCR_RW | HCR_AMO | HCR_IMO | HCR_FMO
    (All exceptions routed to EL2 for KVM handling)
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.