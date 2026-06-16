# `finalise_el2` — VHE and nVHE Finalization Overview

## The Call in `__primary_switched`

```asm
// arch/arm64/kernel/head.S
SYM_FUNC_START(__primary_switched)
    ...
    set_cpu_boot_mode_flag
    ...
#ifdef CONFIG_KASAN
    bl      kasan_early_init
#endif
    mov     x0, x20          // x20 = boot mode (EL1 or EL2)
    bl      finalise_el2     // ← CONFIGURE EL2 OR RETURN
    ...
    bl      start_kernel
SYM_FUNC_END(__primary_switched)
```

`finalise_el2` receives the boot mode in `x0`. Its entire purpose is to
complete the EL2 configuration that was started in `init_kernel_el` (very
early in `primary_entry`). By the time `__primary_switched` calls it, the
kernel has:
1. Set up CPU tasks and stacks
2. Installed exception vectors
3. Saved FDT and kimage_voffset
4. Recorded boot mode

Now EL2 can be finalized safely.

---

## What `finalise_el2` Does

```asm
// arch/arm64/kernel/head.S (conceptual, simplified):
SYM_FUNC_START(finalise_el2)
    // x0 = boot mode
    cbnz    x0, 1f            // if x0 != 0: EL2 boot → configure
    ret                        // EL1 boot: nothing to do
    
1:  // We booted from EL2:
    // Determine if VHE is active (E2H = 1 in HCR_EL2):
    mrs     x1, hcr_el2
    tbz     x1, #HCR_E2H_SHIFT, 2f   // if E2H=0: go to nVHE path
    
    // VHE path (E2H=1):
    // We're running at EL2 with host extensions
    // Set up SPSR_EL2 and ELR_EL2 for eret to EL2 "host EL1" mode
    // (which is what Linux will run as after finalise_el2)
    bl      finalise_el2_vhe
    b       3f
    
2:  // nVHE path (E2H=0):
    // Kernel will stay at EL1
    // Set up EL2 for KVM stub (minimal EL2 presence)
    bl      finalise_el2_nvhe

3:  ret
SYM_FUNC_END(finalise_el2)
```

---

## The Two Paths: VHE vs nVHE

```
                    finalise_el2 called
                           │
                     x0 == 0?
                    /         \
                  YES           NO (EL2 boot)
                   │                │
                  ret         HCR_EL2.E2H?
                               /        \
                              0            1
                           (nVHE)        (VHE)
                              │             │
                   finalise_el2_nvhe   finalise_el2_vhe
                              │             │
                  KVM stub    │    Host EL2 mode setup
                  installed   │    SPSR_EL2 configured
                  at EL2      │    
                              │
                          Both paths → ret
                              │
                      continue to start_kernel
```

---

## Why Finalization Is Deferred to `__primary_switched`

In `init_kernel_el` (very early assembly, before page tables):
- MMU is OFF
- Only minimal EL2 configuration possible
- HCR_EL2 set, VHE detected

In `__primary_switched` (after `__primary_switch` enables MMU):
- MMU is ON, virtual addresses are valid
- Stack is set up (needed for C function calls)
- Exception vectors installed (safe to configure EL2)
- `kimage_voffset` known (needed for VA-to-PA conversions in EL2 setup)

`finalise_el2` needs the MMU to be on because it:
1. Writes to system registers using kernel virtual addresses
2. May call C helper functions (`finalise_el2_vhe` uses the C stack)
3. Needs consistent exception vector state (VBAR_EL1 already set)

---

## ARM64 EL2 System Register State After `finalise_el2`

For **VHE path** (after `finalise_el2_vhe`):
```
HCR_EL2  = HCR_RW | HCR_E2H | HCR_TGE   // EL2 host mode active
VBAR_EL2 = vectors (VHE: EL1 exceptions redirect to EL2)
SCTLR_EL2= SCTLR value for EL2 host mode
CNTHCTL_EL2 = CNTHCTL_EL1PCEN | CNTHCTL_EL1PCTEN  // timer access
CNTVOFF_EL2 = 0  // no virtual timer offset (host mode)
```

For **nVHE path** (after `finalise_el2_nvhe`):
```
HCR_EL2  = HCR_RW | HCR_AMO | HCR_IMO | HCR_FMO  // trap some things to EL2
VBAR_EL2 = __kvm_hyp_vector  // KVM's EL2 exception vector
SCTLR_EL1 = normal EL1 value (Linux continues at EL1)
```

---

## Security Implications of `finalise_el2`

**Without proper EL2 finalization:**
- EL2 registers in indeterminate state
- KVM could trigger unexpected EL2 exceptions
- Spectre-v2 mitigations (which require EL2 configuration) may be incomplete
- Guest VMs could potentially escape to host via malformed EL2 state

**With proper finalization:**
- EL2 in known, controlled state
- `HSTR_EL2` (Hyp System Trap Register) configured to trap guest EL1 operations
  correctly
- `HACR_EL2` configured for implementation-specific traps
- Consistent foundation for subsequent KVM initialization

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VHE (Virtualization Host Extension, ARMv8.1-A, HCR_EL2.E2H=1) allows the kernel to run at EL2 instead of EL1 in a KVM host scenario. When E2H=1, EL1 system register accesses are re-routed to their EL2 equivalents (e.g., MSR SCTLR_EL1 writes to SCTLR_EL2). PSTATE.M determines the current exception level. Without VHE, the hypervisor must context-switch all EL1 registers on every VM entry/exit; with VHE, the host kernel IS the EL2 code and the context-switch overhead is eliminated. The CPU hardware differentiates VHE from non-VHE mode by the HCR_EL2.E2H bit.

### Kernel Perspective (Linux ARM64)
Linux detects VHE capability in __primary_switched by reading ID_AA64MMFR1_EL1.VH. If VHE is available and KVM is configured, the kernel uses finalise_el2 (arch/arm64/kernel/hyp-stub.S) to switch to EL2 before start_kernel. The boot CPU mode flag (x20 bit 0 in __primary_switch: BOOT_CPU_FLAG_E2H) records whether the boot CPU entered VHE mode. Secondary CPUs must match the primary CPU's VHE mode. The is_hyp_mode_available() and is_kernel_in_hyp_mode() helpers in arch/arm64/include/asm/virt.h let the kernel test VHE mode at runtime.

### Memory Perspective (ARMv8 Memory Model)
In VHE mode (EL2), the CPU uses TTBR0_EL2 and TTBR1_EL2 for translation (with HCR_EL2.E2H=1, the EL2 translation regime gains a TTBR1 equivalent). The memory map is the same conceptually but the table root registers are different. Stage 2 translation (GPA->PA) is also controlled by EL2. For a KVM guest, stage 1 (VA->IPA, managed by the guest OS at EL1) and stage 2 (IPA->PA, managed by KVM at EL2) are both active. The ARMv8 memory model handles two-stage translation transparently: the TLB caches combined stage-1 + stage-2 entries.