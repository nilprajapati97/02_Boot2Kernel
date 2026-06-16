# AAPCS64 Callee-Saved Registers and Why `x20`/`x21` Survive All Callees

**Specification:** Procedure Call Standard for AArch64 (AAPCS64), section 5.1  
**Critical question:** How do we know `x20` (boot mode) and `x21` (FDT pointer)
are intact when `__primary_switch` uses them after calling `__enable_mmu`?

---

## 0. The Problem Statement

`__primary_switch` calls two functions before using `x20` and `x21`:

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp    x1, reserved_pg_dir
    adrp    x2, __pi_init_idmap_pg_dir
    bl      __enable_mmu                    // x20, x21 must survive this

    adrp    x1, early_init_stack
    mov     sp, x1
    mov     x29, xzr
    mov     x0, x20     // ← used here: MUST be intact
    mov     x1, x21     // ← used here: MUST be intact
    bl      __pi_early_map_kernel           // x20, x21 must survive this too
    ...
```

If `__enable_mmu` or `__pi_early_map_kernel` clobbers `x20` or `x21`, the
FDT pointer and boot mode are lost forever — and there is no way to recover them.

The answer is: the **AAPCS64 ABI** guarantees these registers are preserved.

---

## 1. The AAPCS64 Register Classification

The AArch64 Procedure Call Standard divides the 31 general-purpose registers
into two classes:

### Caller-Saved (Corruptible) Registers — `x0`–`x18`

| Register | Alternate name | Role |
|---|---|---|
| `x0`–`x7` | — | Function arguments / return values |
| `x8` | `xip0` (indirect result reg) | Indirect result location pointer |
| `x9`–`x15` | — | Temporary (caller saves if needed) |
| `x16`–`x17` | `ip0`, `ip1` | Intra-procedure-call scratch |
| `x18` | Platform register | Platform-specific use (often TLS on some OSes) |

A function is **free to corrupt** any of `x0`–`x18` without saving/restoring them.
The caller must save these to the stack if it needs their values after the call.

### Callee-Saved (Preserved) Registers — `x19`–`x28`, `x29`, `x30`

| Register | Alternate name | Role |
|---|---|---|
| `x19`–`x28` | — | **Must be preserved** by any called function |
| `x29` | `fp` | Frame pointer — **must be preserved** |
| `x30` | `lr` | Link register (return address) — **must be preserved** |

A function that uses any of `x19`–`x28` must save them on entry (via
`stp x19, x20, [sp, #-16]!`) and restore them before returning.

### Special Registers

| Register | Name | Role |
|---|---|---|
| `x31` | `sp` / `xzr` | Stack pointer or zero register (context-dependent) |
| `sp` | — | Stack pointer (cannot use as xzr in most contexts) |

---

## 2. Register Allocation in the Boot Path

Examining the comment block in `head.S`:

```asm
/*
 * The following callee saved general purpose registers are used on the
 * primary lowlevel boot path:
 *
 *  Register   Scope                                           Purpose
 *  x19        primary_entry() .. start_kernel()      MMU state at boot
 *  x20        primary_entry() .. __primary_switch()  CPU boot mode
 *  x21        primary_entry() .. start_kernel()      FDT pointer
 */
```

The kernel deliberately chooses **callee-saved** registers (`x19`, `x20`, `x21`)
for these long-lived values precisely because the AAPCS64 ABI guarantees any
compliant function will preserve them.

---

## 3. Proof: `__enable_mmu` Preserves `x20` and `x21`

Reading `__enable_mmu` in `arch/arm64/kernel/head.S`:

```asm
SYM_FUNC_START(__enable_mmu)
    mrs     x3, ID_AA64MMFR0_EL1          // uses x3 (caller-saved)
    ubfx    x3, x3, #...
    cmp     x3, #...
    b.lt    __no_granule_support
    cmp     x3, #...
    b.gt    __no_granule_support
    phys_to_ttbr x2, x2                   // modifies x2 (caller-saved)
    msr     ttbr0_el1, x2
    load_ttbr1 x1, x1, x3                 // modifies x1, x3 (caller-saved)
    set_sctlr_el1 x0                      // reads x0 (caller-saved)
    ret
SYM_FUNC_END(__enable_mmu)
```

**Registers used:** `x0`, `x1`, `x2`, `x3` — all caller-saved (x0–x18 range).

**Registers untouched:** `x19`, `x20`, `x21`, `x22`–`x28` — none of these are
read, written, pushed, or popped by `__enable_mmu`.

**Conclusion:** `x20` (boot mode) and `x21` (FDT pointer) are guaranteed intact
after `bl __enable_mmu`.

---

## 4. Proof: `__pi_early_map_kernel` Preserves `x20` and `x21`

`__pi_early_map_kernel` is a **C function** compiled from
`arch/arm64/mm/pi/map_kernel.c`. It receives two arguments:

```c
// map_kernel.c (simplified prototype)
void __pi_early_map_kernel(u64 boot_status, void *fdt);
// Called as:
//   x0 = x20 (boot_status)
//   x1 = x21 (fdt)
```

Because it is a C function compiled with the standard toolchain (`aarch64-linux-gnu-gcc`
or Clang), it **must** conform to AAPCS64. The compiler will:

1. Save any of `x19`–`x28` that it uses internally at function prologue.
2. Restore them at epilogue before `ret`.

The function receives its arguments in `x0` and `x1` (not in `x20`/`x21`).
Even if the function internally uses `x19`, `x20`, `x21` for local variables,
it saves and restores them. The caller's (assembly code's) `x20` and `x21`
values are untouched from the caller's perspective.

**What the compiler does internally (conceptual frame):**

```asm
// Compiler-generated prologue for __pi_early_map_kernel
stp     x29, x30, [sp, #-N]!    // save frame pointer and link register
stp     x19, x20, [sp, #...]    // save x19, x20 if used as locals
stp     x21, x22, [sp, #...]    // save x21, x22 if used as locals
mov     x29, sp

// ... function body (may use x0-x18 freely, x19-x28 after saving) ...

// Compiler-generated epilogue
ldp     x21, x22, [sp, #...]    // restore x21, x22
ldp     x19, x20, [sp, #...]    // restore x19, x20
ldp     x29, x30, [sp, #N]!     // restore frame pointer and link register
ret
```

When `bl __pi_early_map_kernel` returns, the assembly caller's `x20` and `x21`
are byte-for-byte identical to what they were before the call.

---

## 5. Why This Design Is Deliberate — Not Accidental

The kernel architects made a deliberate choice: use `x19`–`x21` for
long-lived boot state variables that must survive multiple function calls.

**Alternative approach (not used):** Store to memory, reload from memory.

```asm
// Alternative: store FDT pointer to memory
adr_l   x0, __fdt_pointer
str     x21, [x0]          // save to memory before callees
...
bl      __enable_mmu
...
adr_l   x1, __fdt_pointer
ldr     x21, [x1]          // reload after callees
```

This works but has downsides:
- **Requires the MMU to be on** for a virtual address to be valid, but
  `__fdt_pointer` is a BSS variable in the high VA range.
- **Requires a physical address** before MMU-on, which would require `adr_l`
  and careful cache management.
- **Adds latency** (memory load vs register-to-register).

The ABI-guaranteed callee-saved register approach is cleaner and zero-cost.

---

## 6. The Stack Frame At Each Call Level

At `__primary_switch` entry (immediately after `b __primary_switch` from
`primary_entry`), the stack is:

```
SP_EL1 → early_init_stack (physical address)
x29 = 0 (frame pointer = no parent frame)
x30 = (undefined — set by bl __cpu_setup, but __primary_switch entered by b)
```

When `bl __enable_mmu` is called:
```
SP_EL1 → [no stack space pushed by __primary_switch — it hasn't touched SP yet]
x30 = return address within __primary_switch
```

> **Key observation:** `__primary_switch` itself does NOT push/pop any callee-
> saved registers to the stack before calling `__enable_mmu`. It relies entirely
> on the ABI to preserve `x20` and `x21`. This is safe because:
> 1. `__enable_mmu` uses only x0–x3.
> 2. `__pi_early_map_kernel` conforms to AAPCS64 and preserves x19–x28.

---

## 7. What Would Break If AAPCS64 Were Violated

Suppose a hypothetical buggy `__enable_mmu` that corrupts `x20`:

```asm
// BUGGY __enable_mmu:
...
mov     x20, xzr    // accidental clobber of x20 (AAPCS64 violation!)
...
ret
```

Back in `__primary_switch`:
```asm
mov     x0, x20     // x20 = 0 now! (should be BOOT_CPU_MODE_EL2 = 0xe12)
mov     x1, x21
bl      __pi_early_map_kernel   // receives boot_status = 0 instead of 0xe12
```

`__pi_early_map_kernel` would see `boot_status = 0`, which is neither
`BOOT_CPU_MODE_EL1` (0xe11) nor `BOOT_CPU_MODE_EL2` (0xe12). Depending
on how the function uses this, VHE detection might fail, and later
`finalise_el2` would make wrong decisions about EL2 setup — potentially
leaving the hypervisor infrastructure in a broken state.

---

## 8. Full Register State Accounting Through `__primary_switch`

| Register | At Entry | After `__enable_mmu` | After `__pi_early_map_kernel` | At `br x8` |
|---|---|---|---|---|
| `x0` | SCTLR_EL1_MMU_ON | clobbered | receives boot_status; clobbered | `__pa(KERNEL_START)` |
| `x1` | reserved_pg_dir | clobbered | receives FDT; clobbered | clobbered |
| `x2` | idmap_pg_dir | clobbered | clobbered | clobbered |
| `x3`–`x18` | undefined | clobbered | clobbered | clobbered |
| `x19` | MMU-at-boot flag | **preserved** | **preserved** | **preserved** |
| `x20` | CPU boot mode | **preserved (ABI)** | **preserved (ABI)** | **preserved** |
| `x21` | FDT physical addr | **preserved (ABI)** | **preserved (ABI)** | **preserved** |
| `x8` | undefined | undefined | undefined | `__primary_switched` VA |
| `sp` | early_init_stack(PA) | unchanged | reset to early_init_stack(PA) again | same |
| `x29` | 0 | 0 | 0 | 0 |

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