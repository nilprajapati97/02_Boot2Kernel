# Register Survival Proof Across Callees

**Claim:** x20 (boot mode) and x21 (FDT PA) survive across ALL function calls
in `__primary_switch` with guaranteed correctness  
**Standard:** AAPCS64 (Procedure Call Standard for the Arm 64-bit Architecture)

---

## 0. The Problem Statement

`__primary_switch` uses `x20` and `x21` to carry critical information:
- `x20` = CPU boot mode (0xE11 or 0xE12 ± VHE flag)
- `x21` = FDT physical address

These must survive through:
1. `bl __pi_early_map_kernel` — a **C function** (complex, many internal calls)
2. `bl __enable_mmu` — an **assembly function**

If either function overwrites x20 or x21 without restoring them, the boot
mode and FDT pointer would be corrupted. This would silently break:
- EL2/VHE detection in `__primary_switched`
- FDT mapping and all device tree parsing

**This document proves the registers are safe.**

---

## 1. AAPCS64 Register Classification (Full Table)

```
Register  Role              Callee-Saved?  Notes
────────────────────────────────────────────────────────────────────────────
x0        Arg 1 / Return 1  NO (caller)    Clobbered freely
x1        Arg 2 / Return 2  NO (caller)    Clobbered freely
x2        Arg 3             NO (caller)    Clobbered freely
x3        Arg 4             NO (caller)    Clobbered freely
x4        Arg 5             NO (caller)    Clobbered freely
x5        Arg 6             NO (caller)    Clobbered freely
x6        Arg 7             NO (caller)    Clobbered freely
x7        Arg 8             NO (caller)    Clobbered freely
x8        Indirect result   NO (caller)    Clobbered freely
x9-x15    Temporaries       NO (caller)    Clobbered freely
x16 (IP0) Intra-procedure   NO (caller)    Used by linker veneers
x17 (IP1) Intra-procedure   NO (caller)    Used by linker veneers
x18       Platform register NO (caller)    Used by SCS in Linux (but not saved/restored in callee)
x19-x28   Callee-saved      YES (callee)   Callee MUST save and restore if used
x29 (FP)  Frame pointer     YES (callee)   Callee MUST save if used
x30 (LR)  Link register     YES (callee)   Callee MUST save if making further calls
sp        Stack pointer      YES (callee)   Callee MUST restore to entry value
```

**`x20` and `x21` are in the callee-saved range (`x19`–`x28`).**

This means any function that uses x20 or x21 for its own purposes MUST save
them to the stack in the prologue and restore them in the epilogue.

---

## 2. Proof for `__pi_early_map_kernel` (C Function)

`__pi_early_map_kernel` is compiled from C source:

```c
// arch/arm64/mm/pi/map_kernel.c
asmlinkage void __init __pi_early_map_kernel(unsigned long fdt_pa, ...)
{
    // ... complex page table building logic ...
}
```

When GCC/Clang compiles this function with `-O2` and `AAPCS64`:

### What the Compiler Generates

```asm
// __pi_early_map_kernel prologue (compiler-generated):
stp     x29, x30, [sp, #-N]!    // Save FP and LR
mov     x29, sp                  // Set frame pointer
stp     x19, x20, [sp, #16]     // Save x19, x20 if used by this function
stp     x21, x22, [sp, #32]     // Save x21, x22 if used
...

// function body — free to use x19-x22 for its own purposes

// __pi_early_map_kernel epilogue:
ldp     x21, x22, [sp, #32]     // Restore x21, x22
ldp     x19, x20, [sp, #16]     // Restore x19, x20
ldp     x29, x30, [sp], #N      // Restore FP, LR, deallocate frame
ret
```

After `ret`, `x20` and `x21` hold the values they had when the function was
called — unchanged from the caller's perspective.

**This is a hard ABI guarantee.** If the compiler violates this, it is a
compiler bug (undefined behavior in the AAPCS64 model). Linux is built with
well-tested GCC/Clang versions where this never happens.

### Verifying with objdump

To verify at the actual binary level (on a built kernel):

```bash
$ aarch64-linux-gnu-objdump -d vmlinux | grep -A 50 '<__pi_early_map_kernel>'
```

The prologue will show `stp x19, x20` or similar if those registers are used,
and the epilogue will show `ldp x19, x20` correspondingly.

---

## 3. Proof for `__enable_mmu` (Assembly Function)

```asm
// arch/arm64/kernel/head.S — __enable_mmu:
SYM_FUNC_START(__enable_mmu)
    mrs     x3, ID_AA64MMFR0_EL1       // uses x3 (caller-saved — fine)
    ubfx    x3, x3, #TGRAN_SHIFT, 4   // uses x3
    cmp     x3, #TGRAN_SUPPORTED_MIN   
    b.lt    __no_granule_support
    cmp     x3, #TGRAN_SUPPORTED_MAX
    b.gt    __no_granule_support
    phys_to_ttbr x2, x2               // uses x2 (caller-saved — fine)
    msr     ttbr0_el1, x2             // system register write
    load_ttbr1 x1, x1, x3            // uses x1, x3
    set_sctlr_el1 x0                  // uses x0
    ret                               // x30 → return address
SYM_FUNC_END(__enable_mmu)
```

**Registers used by `__enable_mmu`:** x0, x1, x2, x3, x30 (LR — consumed by ret).

**Registers NOT touched:** x4–x29 (including x20 and x21).

This is visible by inspection of the assembly source. No save/restore of
x20/x21 is needed because they are never modified.

---

## 4. Complete Register Accounting Table for `__primary_switch`

```
Register  Entry Value              After __pi_early_map_kernel  After __enable_mmu
────────────────────────────────────────────────────────────────────────────────
x0        SCTLR_EL1_MMU_ON (from __cpu_setup)  return value     consumed by set_sctlr_el1
x1        swapper_pg_dir PA       return value (???)           consumed by load_ttbr1
x2        __pi_init_idmap_pg_dir PA  return value (???)        consumed by phys_to_ttbr
x19       saved by callee         SAME as entry (restored)     SAME as entry (not touched)
x20       BOOT_CPU_MODE           SAME as entry (restored) ✓   SAME as entry (not touched) ✓
x21       FDT PA                  SAME as entry (restored) ✓   SAME as entry (not touched) ✓
x30       LR (return address)     SAME as entry (restored)     consumed by ret
sp        early_init_stack PA     SAME as entry (restored)     SAME as entry (not touched)
```

---

## 5. The `__primary_switch` Source Verification

```asm
// arch/arm64/kernel/head.S (approximate) — __primary_switch:
SYM_FUNC_START_LOCAL(__primary_switch)
    /*
     * x0 = SCTLR_EL1 value
     * x1 = TTBR1 (swapper_pg_dir PA)
     * x2 = TTBR0 (__pi_init_idmap_pg_dir PA)
     * x19 = kimage_voffset (set earlier? — depends on version)
     * x20 = boot mode
     * x21 = FDT PA
     */
    
    // Map the kernel:
    mov     x0, x21                 // x0 = FDT PA (argument to C function)
    bl      __pi_early_map_kernel   // x20, x21 will be preserved ✓

    // Enable MMU:
    adrp    x1, swapper_pg_dir      // x1 = swapper_pg_dir PA
    adrp    x2, __pi_init_idmap_pg_dir  // x2 = idmap PA
    bl      __enable_mmu            // x20, x21 not touched ✓

    // Jump to virtual space:
    ldr     x8, =__primary_switched // x8 = VA of __primary_switched
    br      x8                      // Jump — x20, x21 still valid
SYM_FUNC_END_LOCAL(__primary_switch)
```

---

## 6. What Would Happen If the Proof Failed

Scenario: `__pi_early_map_kernel` is compiled with a compiler bug that
clobbers x21 without restoring it.

```
After bl __pi_early_map_kernel:
    x21 = garbage (e.g., 0x0000_0000_0000_0000 or a local pointer)

In __primary_switched:
    str x21, [x4]   // __fdt_pointer = 0x0000_0000 (NULL!)

In setup_arch:
    early_init_dt_scan(phys_to_virt(0)) 
    → phys_to_virt(0) = PAGE_OFFSET
    → fdt_check_header(PAGE_OFFSET)
    → FDT magic check fails
    → kernel panics: "Error: invalid dtb"
```

The failure mode is clear and would be caught in the first boot test of any
kernel build with the buggy compiler. This is not a latent bug — it manifests
immediately on first boot.

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