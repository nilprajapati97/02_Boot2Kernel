# x20 — CPU Boot Mode Complete Reference

**Register:** `x20`  
**Value:** Saved from `x0` in `primary_entry`, passed through all of `__primary_switch`  
**Source:** `arch/arm64/include/asm/virt.h` — `BOOT_CPU_MODE_*` constants

---

## 0. Why x20?

In `primary_entry`:

```asm
// arch/arm64/kernel/head.S
SYM_CODE_START(primary_entry)
    ...
    mov     x20, x0             // Save boot mode (passed from __cpu_setup via x19/x20 convention)
```

Actually, in the real head.S, the boot mode (`x0` = BOOT_CPU_MODE_EL1 or
BOOT_CPU_MODE_EL2) is saved into `x20` at the `primary_entry` level and preserved
across all calls because `x20` is a **callee-saved register** (AAPCS64
`x19`–`x28` must be preserved by callees).

---

## 1. The Boot Mode Values

Defined in `arch/arm64/include/asm/virt.h`:

```c
// arch/arm64/include/asm/virt.h — lines 54-62

#define BOOT_CPU_MODE_EL1   (0xe11)
#define BOOT_CPU_MODE_EL2   (0xe12)

/*
 * This flag is set when the boot CPU entered EL2 and activated VHE
 * (Virtualization Host Extensions) mode.
 */
#define BOOT_CPU_FLAG_E2H   BIT_ULL(32)
```

### Value Semantics

| Value | Hex | Binary (low bits) | Meaning |
|---|---|---|---|
| `BOOT_CPU_MODE_EL1` | `0x0000_0000_0000_0E11` | ...1110_0001_0001 | CPU started in EL1 |
| `BOOT_CPU_MODE_EL2` | `0x0000_0000_0000_0E12` | ...1110_0001_0010 | CPU started in EL2 |
| `BOOT_CPU_MODE_EL2 \| BOOT_CPU_FLAG_E2H` | `0x0000_0001_0000_0E12` | ... | EL2 with VHE active |

The values `0xE11` and `0xE12` were chosen to be:
1. Unique (not accidentally equal to any valid register value or pointer)
2. Distinguishable from each other in the low 4 bits (`1` vs `2`)
3. Human-readable in hex (`E11` ≈ "EL1", `E12` ≈ "EL2")

### `BOOT_CPU_FLAG_E2H` — BIT_ULL(32)

This flag uses bit 32 of the 64-bit value — the **upper 32 bits** of x20. This
is clever because:
- `BOOT_CPU_MODE_EL2` uses bits[11:0] only
- `BOOT_CPU_FLAG_E2H` uses bit[32] only
- They can be OR'd together without conflict
- Testing for each is independent

```c
// Test: is VHE active?
if (boot_mode & BOOT_CPU_FLAG_E2H) {
    // CPU booted at EL2 with VHE (Virtualization Host Extensions)
}

// Test: what EL did we start at?
switch (boot_mode & 0xFFFF) {
    case BOOT_CPU_MODE_EL1: /* EL1 */ break;
    case BOOT_CPU_MODE_EL2: /* EL2 */ break;
}
```

---

## 2. How x20 Is Set

```asm
// arch/arm64/kernel/head.S — primary_entry:
bl      __cpu_setup             // x0 = SCTLR_EL1_MMU_ON value on return
                                // x1 = boot mode (set earlier by el2_setup)

// Earlier, from el2_setup or the EL level detection:
// x20 = BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2 | flags
```

The actual setup of `x20` happens in `el2_setup` and `init_kernel_el`:

```asm
// If CPU entered at EL1:
mov     x20, #BOOT_CPU_MODE_EL1         // x20 = 0xE11

// If CPU entered at EL2 (normal, no VHE):
mov     x20, #BOOT_CPU_MODE_EL2         // x20 = 0xE12

// If CPU entered at EL2 with VHE (E2H bit in HCR_EL2):
mov     x20, #BOOT_CPU_MODE_EL2
orr     x20, x20, #BOOT_CPU_FLAG_E2H    // x20 = 0x1_0000_0E12
```

---

## 3. Where x20 Is Used

### 3.1 In `__primary_switched`

```asm
// arch/arm64/kernel/head.S — __primary_switched:
...
adr_l   x4, __boot_cpu_mode     // Address of storage variable
cmp     x20, #BOOT_CPU_MODE_EL2 // Test if we booted from EL2
b.ne    1f                       // If not EL2, skip
...
str     x20, [x4]                // Store boot mode to __boot_cpu_mode
```

The `__boot_cpu_mode` variable stores the mode for later kernel use:

```c
// arch/arm64/include/asm/virt.h
extern u64 __boot_cpu_mode;     // Defined in head.S; readable from C
```

### 3.2 In `is_hyp_mode_available()`

```c
// arch/arm64/include/asm/virt.h
static inline bool is_hyp_mode_available(void)
{
    return (__boot_cpu_mode & BOOT_CPU_MODE_EL2) == BOOT_CPU_MODE_EL2;
}
```

This is checked when:
- KVM initializes (`kvm_arch_init`)
- The kernel decides whether to enable EL2 hypervisor features
- Setting up IOMMU with stage-2 translation

### 3.3 In `is_kernel_in_hyp_mode()`

```c
static inline bool is_kernel_in_hyp_mode(void)
{
    return read_sysreg(CurrentEL) == CurrentEL_EL2;
}
```

When `BOOT_CPU_FLAG_E2H` is set, the kernel actually runs at EL2 (VHE mode).
This is the **nVHE vs. VHE** distinction.

---

## 4. VHE (Virtualization Host Extensions) — What It Means

### ARMv8.0 (No VHE)

```
EL3: Firmware (TF-A / EL3 monitor)
EL2: Hypervisor (e.g., KVM)
EL1: Guest OS kernel / host Linux kernel
EL0: Guest userspace / host userspace
```

Without VHE, Linux runs at EL1. This means:
- Linux cannot use EL2 instructions/registers directly
- KVM (if loaded) must drop from EL2 to EL1 to run host Linux code

### ARMv8.1 VHE (`HCR_EL2.E2H = 1`)

```
EL3: Firmware
EL2: Host Linux kernel (elevated from EL1!)
EL1: Guest OS kernel
EL0: Host userspace / Guest userspace
```

With VHE, the host Linux kernel runs at EL2. Benefits:
- Linux can use EL2 system registers directly
- KVM doesn't need to context-switch between EL2 and EL1 for host code
- Reduced KVM overhead

**BOOT_CPU_FLAG_E2H in x20** signals that VHE was activated during `el2_setup`.

---

## 5. How x20 Survives Across Function Calls

`x20` is preserved across `__enable_mmu` because:
1. AAPCS64 designates `x19`–`x28` as callee-saved
2. `__enable_mmu` is written in assembly and only modifies `x0`–`x3` and `x30`
3. Neither `__pi_early_map_kernel` (C function) nor `__enable_mmu` may clobber x20

The compiler-generated `__pi_early_map_kernel` function:
- If it uses `x20`, it must save it in the prologue (`stp x20, x21, [sp, #N]`)
- And restore it in the epilogue (`ldp x20, x21, [sp, #N]`)
- This is guaranteed by the compiler

The assembly `__enable_mmu`:
- Reading the source: it uses `x0`, `x1`, `x2`, `x3` only
- `x20` is never touched

---

## 6. Secondary CPU Boot Mode

Secondary CPUs follow a different path (`secondary_startup`) but also end up
at `__secondary_switched`. They call `__cpu_setup` which sets the same boot
mode (each CPU independently detects its starting EL).

The `__boot_cpu_mode` variable stores **the primary CPU's** boot mode. Secondary
CPUs verify their mode matches:

```asm
// arch/arm64/kernel/head.S — secondary_startup:
ldr     x20, __boot_cpu_mode    // Load primary CPU's boot mode
...
// verify current EL matches x20
```

A mismatch (primary booted from EL2 but secondary from EL1) would be a system
configuration error.

---

## 7. Debug: Checking x20 in Crash Dumps

When analyzing a kernel crash dump with `crash` or `gdb`:

```
(crash) bt
```

If the crash happened in early boot (before `start_kernel`), the register dump
shows:

```
x20: 0x0000000000000e12   → BOOT_CPU_MODE_EL2 (normal)
x20: 0x0000000100000e12   → BOOT_CPU_MODE_EL2 | BOOT_CPU_FLAG_E2H (VHE)
x20: 0x0000000000000e11   → BOOT_CPU_MODE_EL1 (no EL2 — rare on server hardware)
```

Any other value in `x20` at early boot indicates a bug in `el2_setup` or
`init_kernel_el`.

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