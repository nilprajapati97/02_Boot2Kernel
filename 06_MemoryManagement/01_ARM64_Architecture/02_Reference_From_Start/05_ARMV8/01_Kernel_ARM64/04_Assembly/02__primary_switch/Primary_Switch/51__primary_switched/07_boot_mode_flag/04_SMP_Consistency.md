# SMP Boot Mode Consistency — Why All CPUs Must Match

## The SMP Boot Mode Requirement

On a multi-CPU system, ALL CPUs must boot from the SAME exception level.
Mixing EL1 and EL2 boots is a FATAL firmware bug.

Why? Because:
1. KVM requires EL2 access on ALL CPUs to work (can't virtualize with only some CPUs)
2. VHE mode (E2H=1 in HCR_EL2) must be consistent across all CPUs
3. Exception routing (HCR_EL2 register) must be uniformly configured

---

## The `__boot_cpu_mode` Array

```c
// arch/arm64/include/asm/virt.h
extern u32 __boot_cpu_mode[2];

// Semantics:
// __boot_cpu_mode[0] = BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2
// __boot_cpu_mode[1] = same or BOOT_CPU_MODE_EL2 (secondary mode)
// OR: if both slots have the same value → all CPUs consistent
```

Primary CPU sets `__boot_cpu_mode[0]` in `__primary_switched`.
Secondary CPUs set `__boot_cpu_mode[1]` (or check consistency).

After all CPUs are up, `check_boot_cpu_mode` verifies:
```c
static void __init check_boot_cpu_mode(void)
{
    u32 mode = __boot_cpu_mode[0];
    
    // Check secondary mode matches:
    if (__boot_cpu_mode[0] != __boot_cpu_mode[1]) {
        pr_err("CPU%d: booted in different EL (%s vs %s)\n", ...);
        // This is a fatal inconsistency
    }
}
```

---

## `set_cpu_boot_mode_flag` for Secondary CPUs

```asm
// arch/arm64/kernel/head.S (secondary path):
secondary_startup:
    ...
    // At this point, x20 = current boot mode (same logic as primary)
    mov     x0, x20
    bl      set_cpu_boot_mode_flag    // Store this CPU's boot mode
    ...
```

`set_cpu_boot_mode_flag` for secondary CPUs stores to a different slot or
does a consistency check:
```asm
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
    adr_l   x1, __boot_cpu_mode
    cmp     w0, #BOOT_CPU_MODE_EL2
    b.ne    1f
    add     x1, x1, #4          // Use second slot for EL2 secondary
1:  str     w0, [x1]
    dmb     sy
    dc      civac, x1
    ret
SYM_FUNC_END(set_cpu_boot_mode_flag)
```

---

## ARM64 CPU Hotplug and Boot Mode

CPU hotplug (runtime add/remove of CPUs) also checks boot mode:
```c
// arch/arm64/kernel/smp.c
static int secondary_start_kernel(void)
{
    ...
    // Verify this CPU's boot mode matches primary:
    if (__boot_cpu_mode[0] != get_current_cpu_boot_mode()) {
        pr_err("CPU%d: booted at wrong EL\n", cpu);
        return -EINVAL;
    }
    ...
}
```

A CPU that comes online with the wrong EL is rejected. This can happen if:
- Firmware has a bug and brings a secondary CPU to EL1 while primary was EL2
- ACPI CPU hotplug with inconsistent EL initialization

---

## Real-World Scenario: Asymmetric Boot Mode

Some older ARM64 systems had firmware bugs where:
- big cores (Cortex-A75) booted at EL2
- LITTLE cores (Cortex-A55) booted at EL1

Result:
```
dmesg output:
CPU0: booted in EL2
CPU1: booted in EL2
CPU4: booted in EL1   ← LITTLE core, firmware bug
CPU5: booted in EL1
CPU6: booted in EL1
CPU7: booted in EL1

Kernel: FATAL: Inconsistent CPU boot modes
KVM: disabled (some CPUs lack EL2 access)
```

The `set_cpu_boot_mode_flag` mechanism allows the kernel to detect and report
this firmware bug early, rather than having mysterious failures when KVM is
first used.

---

## `is_hyp_mode_available()` in Practice

```c
// arch/arm64/include/asm/virt.h
static inline bool is_hyp_mode_available(void)
{
    return (__boot_cpu_mode[0] == BOOT_CPU_MODE_EL2 &&
            __boot_cpu_mode[1] == BOOT_CPU_MODE_EL2);
}
```

This is used throughout the kernel:
```c
// arch/arm64/kvm/arm.c:
if (!is_hyp_mode_available()) {
    kvm_err("HYP mode not available\n");
    return -ENODEV;    // KVM module cannot load
}

// arch/arm64/include/asm/mmu.h:
static inline bool arm64_use_ng_mappings(void)
{
    return rodata_enabled || is_hyp_mode_available();
    // Non-global TLB entries needed if VMs exist
}
```

`is_hyp_mode_available()` is consulted during kernel initialization (when KVM
module is loaded) and at various feature detection points. The correctness of
this function depends on `set_cpu_boot_mode_flag` being called correctly in
`__primary_switched`.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, general-purpose registers x0-x30 are each 64-bit. Registers x19-x28 are callee-saved per AAPCS64: a function that modifies them must save them on entry and restore them before returning. The boot registers x20 (CPU boot mode) and x21 (FDT pointer) are chosen from the callee-saved range so that they survive through BL __pi_early_map_kernel and BL __enable_mmu without needing to be pushed/popped on the stack. The hardware provides 31 general-purpose registers (x0-x30) plus XZR (always-zero) and SP (stack pointer).

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot register convention in __primary_switch:
- x20: CPU boot mode flags (BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2, and E2H flag).
- x21: FDT physical address (passed from bootloader in x1, saved early in head.S).
- x22: kernel image physical address (phys_offset).
- x23: init_task VA (for SP_EL0 setup).
- x24: TTBR1_EL1 value (kernel page table root PA).
x20 survives all C calls via the callee-save guarantee; it is read in __primary_switched to propagate the boot mode to __boot_cpu_mode (per-CPU variable).

### Memory Perspective (ARMv8 Memory Model)
The callee-saved registers (x19-x28) act as a free "register file" for passing state across function calls without touching memory. This is particularly important in the pre-MMU phase where there is no reliable stack VA. x21 (FDT PA) is kept in a register rather than memory because the stack is at a physical address that may not be mapped after the MMU is enabled. The register file is part of the CPU's architectural state -- it is not cache-coherent (it is in the register file, not RAM) -- so register passing has zero memory latency.