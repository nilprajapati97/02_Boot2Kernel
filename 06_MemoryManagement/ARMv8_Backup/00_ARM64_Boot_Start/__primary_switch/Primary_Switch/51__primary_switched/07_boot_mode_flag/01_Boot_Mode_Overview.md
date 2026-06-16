# Boot Mode Flag — `set_cpu_boot_mode_flag`

## The Assembly Call

```asm
// arch/arm64/kernel/head.S __primary_switched:
mov     x0, x20                 // x0 = boot mode (EL1 or EL2)
bl      set_cpu_boot_mode_flag  // record which EL we booted from
```

`x20` was set earlier in the boot path to record whether the CPU entered from
EL2 (hypervisor level) or EL1 (operating system level).

---

## What `x20` Holds — The Boot Mode

ARM64 has four Exception Levels:
- EL0: User space
- EL1: OS kernel (Linux runs here)
- EL2: Hypervisor (KVM, Xen)
- EL3: Secure Monitor (ARM TrustZone firmware)

Modern ARM64 platforms boot at **EL2** (hypervisor level) because:
1. Hardware virtualization features require initialization at EL2
2. The CPU resets to EL2 by default on most platforms
3. KVM (Linux's built-in hypervisor) needs to set up at EL2 before dropping to EL1

Older or simpler platforms may boot at **EL1** directly.

`x20` encodes this:
```asm
// From arch/arm64/kernel/head.S init_kernel_el:
    mrs     x0, CurrentEL         // read current exception level
    cmp     x0, #CurrentEL_EL2   // EL2 or EL1?
    b.ne    1f                    // branch if not EL2
    
    // Running at EL2:
    mov_q   x0, INIT_PSTATE_EL2
    msr     spsr_el2, x0
    eret                          // drop to EL1
    
1:  // Running at EL1:
    ...
    
// x20 = ARM64_KERNEL_USES_HYP (2) for EL2 boot
// x20 = ARM64_KERNEL_USES_EL1 (1) for EL1 boot
// (exact values are enum in arch/arm64/include/asm/el2_setup.h)
```

---

## The `set_cpu_boot_mode_flag` Function

```c
// arch/arm64/kernel/setup.c (simplified)
void __init set_cpu_boot_mode_flag(int boot_mode)
{
    /*
     * Record this CPU's boot mode. This is used to detect
     * inconsistencies in how CPUs booted (some at EL1, some at EL2)
     * which would indicate a firmware bug.
     */
    __boot_cpu_mode[0] = boot_mode;
    __boot_cpu_mode[1] = ~boot_mode;  // complement for verification
}
```

```asm
// arch/arm64/kernel/head.S (actual implementation — it's also in assembly)
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
    adr_l   x1, __boot_cpu_mode
    cmp     w0, #BOOT_CPU_MODE_EL2
    b.ne    1f
    add     x1, x1, #4                 // secondary entry offset
1:  str     w0, [x1]
    dmb     sy
    dc      civac, x1                  // clean cache
    ret
SYM_FUNC_END(set_cpu_boot_mode_flag)
```

---

## `__boot_cpu_mode` Variable

```c
// arch/arm64/include/asm/virt.h
extern u32 __boot_cpu_mode[2];

// Initialized to:
// __boot_cpu_mode[0] = 0 initially
// __boot_cpu_mode[1] = 0 initially
```

After `set_cpu_boot_mode_flag`:
```
__boot_cpu_mode[0] = BOOT_CPU_MODE_EL1 (0x0e11) or BOOT_CPU_MODE_EL2 (0x0e12)
__boot_cpu_mode[1] = same value (primary boot)
```

For secondary CPUs:
- Each secondary CPU also calls `set_cpu_boot_mode_flag`
- If any secondary boots at a DIFFERENT EL than the primary, a warning is issued

---

## Why Record the Boot Mode?

1. **KVM decision**: `kvm_arch_init()` checks `__boot_cpu_mode[0]`:
   - If EL2: KVM can use hardware virtualization (full VHE mode)
   - If EL1: KVM cannot run (no hypervisor access); or must use NVHE (non-VHE)

2. **CPU consistency check**: all CPUs on an SMP system MUST boot from the same EL.
   Mixing EL1 and EL2 boots indicates firmware bug → kernel issues warning.

3. **Feature availability**: Some ARM64 features (like Stage 2 MMU) only work if
   the kernel booted from EL2.

4. **`is_hyp_mode_available()`**: the kernel uses this to gate EL2-only features:
   ```c
   bool is_hyp_mode_available(void)
   {
       return (__boot_cpu_mode[0] == BOOT_CPU_MODE_EL2);
   }
   ```

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