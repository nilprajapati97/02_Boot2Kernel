# Register State at Entry — Every Register Explained

## Complete Register Map at Entry to `__primary_switched`

### General Purpose Registers — Callee-Saved (x19-x28)

These registers are preserved by every function per AAPCS64. They carry boot-time
values intact through the entire chain from `primary_entry` to here.

| Reg | Value | Set By | Content |
|---|---|---|---|
| x19 | `mmu_enabled_at_boot` | `record_mmu_state` | 0=MMU off at boot, nonzero=M-bit was set |
| x20 | `cpu_boot_mode` | `init_kernel_el` return | `BOOT_CPU_MODE_EL1(1)` or `BOOT_CPU_MODE_EL2(2)` |
| x21 | FDT physical address | `preserve_boot_args` | PA of DTB blob in RAM |
| x22-x28 | undefined / zero | never set on this path | do NOT read — unpredictable |

---

### Special-Purpose Registers

| Reg | Value | Set By | Content |
|---|---|---|---|
| x0 | `__pa(KERNEL_START)` | `adrp` in `__primary_switch` | Physical base of loaded kernel image |
| x29 | `xzr` (0) | `mov x29, xzr` in `__primary_switch` | Null frame pointer (no prior frame) |
| x30 (LR) | return address to `__primary_switch` | `br x8` doesn't set LR | LR from before `__pi_early_map_kernel` call — NEVER USED |
| SP | `early_init_stack` top | `mov sp, x1` in `__primary_switch` | Temporary boot stack |
| PC | virtual `__primary_switched` | `br x8` | In `.text` section, TTBR1 range |

---

### System Registers

| Register | Value | Meaning |
|---|---|---|
| `SCTLR_EL1` | M=1, C=1, I=1 | MMU ON, D-cache ON, I-cache ON |
| `TTBR0_EL1` | `&idmap_pg_dir` | Identity page table (PA≈VA, low range) |
| `TTBR1_EL1` | `&swapper_pg_dir` | Kernel page table (high VA range) |
| `TCR_EL1` | configured | VA range, granule, cacheability |
| `MAIR_EL1` | attribute indices | Cache/device memory attributes |
| `VBAR_EL1` | **0 or garbage** | **UNSAFE — no valid exception handler** |
| `SP_EL0` | undefined | Not yet set — `current` is INVALID until `init_cpu_task` |
| `TPIDR_EL1` | undefined | Not yet set — per-CPU INVALID until `init_cpu_task` |
| `CurrentEL` | EL1 or EL2 | Determined by bootloader/ATF |
| `SPSR_EL1/2` | from `init_kernel_el` | Saved program state for ERET |

---

## Deep Dive: x0 = `__pa(KERNEL_START)` — How It Was Computed

This register value is the most precisely computed value in the boot path.

**In `__primary_switch`, the sequence is:**
```asm
bl    __pi_early_map_kernel   // PC still in TTBR0 range (identity mapped)
                               // after this, kernel is at (possibly randomized) PA+VA

ldr   x8, =__primary_switched // load absolute VA of __primary_switched
adrp  x0, KERNEL_START        // ← THIS IS THE KEY INSTRUCTION
br    x8
```

**Why `adrp` gives a physical address here:**
- At the `adrp x0, KERNEL_START` instruction, PC is STILL in `.idmap.text` (identity-mapped)
- In identity-mapped space, VA ≈ PA (the identity map maps PA to the same-numbered VA)
- So `adrp` which computes `PC_aligned + offset` gives a value where the result
  is the same number as the physical address

**What `KERNEL_START` is:**
```c
// arch/arm64/kernel/image.h
#define KERNEL_START    _text
```
`_text` is the first symbol in the kernel image, defined in the linker script.
Its physical address = wherever the bootloader loaded the kernel Image file.

**KASLR scenario:**
Without KASLR: `_text` PA = `CONFIG_PHYSICAL_START` (typically `0x40080000` or `0x80000`)
With KASLR: `_text` PA = random base chosen by `__pi_early_map_kernel`

In both cases, the `adrp` instruction at that exact PC location correctly reflects
the current physical load base.

---

## Deep Dive: x20 = `cpu_boot_mode` — Bit-Level Analysis

```
x20 = return value of init_kernel_el()

Format:
  bits [63:32] = context flags (high half — NOT stored in __boot_cpu_mode)
  bits [31:0]  = BOOT_CPU_MODE_EL1 (0x1) or BOOT_CPU_MODE_EL2 (0x2)

Context flag:
  BOOT_CPU_FLAG_E2H (bit 32) = set if HCR_EL2.E2H=1 (VHE is active)
```

**How it's used in `__primary_switched`:**
```asm
// set_cpu_boot_mode_flag:
mov  x0, x20         // x0 = full 64-bit value including flags
bl   set_cpu_boot_mode_flag
// Inside: str w0, [x1]  ← writes ONLY lower 32 bits
// The BOOT_CPU_FLAG_E2H in bit 32 is NOT stored — it's transient

// finalise_el2:
mov  x0, x20         // passes full value including E2H flag
bl   finalise_el2
// Inside: checks both the EL value AND the E2H flag
```

---

## Deep Dive: x21 = FDT Physical Address — The Bootloader Contract

The ARM64 Linux boot protocol (Documentation/arm64/booting.rst) defines:

```
At kernel entry:
  x0 = physical address of device tree blob (dtb) in system RAM
  x1 = 0 (reserved for future use)
  x2 = 0 (reserved for future use)
  x3 = 0 (reserved for future use)
```

The first kernel instruction `preserve_boot_args`:
```asm
mov  x21, x0     // x21 = FDT PA (bootloader guaranteed)
adr_l x0, boot_args
stp  x21, x1, [x0]    // also save to boot_args[] for debugging
stp  x2, x3, [x0, #16]
```

**Proof of callee-save survival:**
The register x21 is used as callee-saved across:
1. `__pi_create_init_idmap` — ARM64 C function, must preserve x19-x28
2. `init_kernel_el` — assembly, uses callee-saved x20 but not x21
3. `__cpu_setup` — assembly, does not use x21
4. `__primary_switch` — assembly, does not use x21
5. `__enable_mmu` — assembly, does not use x21
6. `__pi_early_map_kernel` — C function, must preserve x19-x28

**At `__primary_switched`:** x21 still = original FDT PA from bootloader.

---

## Register State AFTER `init_cpu_task` Runs (First 3 lines)

After the first 3 lines of `__primary_switched` execute, the register state changes:

```
Before:                          After init_cpu_task:
SP = early_init_stack (top)      SP = init_stack top - PT_REGS_SIZE
x29 = 0 (from __primary_switch) x29 = sp + S_STACKFRAME (points into pt_regs)
sp_el0 = undefined               sp_el0 = &init_task
tpidr_el1 = undefined            tpidr_el1 = __per_cpu_offset[0]
x4 = &init_task                  x4 = per_cpu_offset[0] (clobbered by macro)
x5 = undefined                   x5 = clobbered (tmp1)
x6 = undefined                   x6 = clobbered (tmp2)
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
This document describes a stage in the ARMv8-A Linux ARM64 boot path. ARMv8-A is the 64-bit ARM architecture (AArch64 execution state) introduced with the ARM Cortex-A53/A57 generation. Key architectural features relevant to boot:
- Exception levels: EL0 (user), EL1 (OS kernel), EL2 (hypervisor), EL3 (secure monitor).
- Two-stage translation: TTBR0_EL1 (user/low VA) and TTBR1_EL1 (kernel/high VA).
- System registers accessed via MRS/MSR instructions (not memory-mapped).
- PSTATE: condition flags + CPU mode + interrupt mask bits.
- Mandatory ISB after system register writes that affect instruction fetch.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot path follows this sequence:
  stext (head.S) -> __primary_switch -> __pi_early_map_kernel -> __enable_mmu
  -> __primary_switched -> start_kernel -> setup_arch -> paging_init
Each stage initializes one more layer of the memory system. Before start_kernel, all memory management is done with physical addresses or the early identity/kernel maps. After paging_init(), the full kernel virtual memory map is active.

### Memory Perspective (ARMv8 Memory Model)
The ARMv8 memory model (based on the ARM ARM's "Arm Memory Model" chapter) defines:
- Normal memory: cacheable, reorderable, speculatable. Used for DRAM (kernel code, data, stack, heap).
- Device memory: non-cacheable, strictly ordered. Used for MMIO (UART, GIC, etc.).
- Barriers: DSB (Data Synchronization Barrier), DMB (Data Memory Barrier), ISB (Instruction Synchronization Barrier) enforce ordering guarantees.
At boot, the kernel transitions from a world where every address is physical (pre-MMU) to the full ARMv8 virtual memory model where TTBR0 and TTBR1 map the user and kernel address spaces respectively.