# AArch64 vs AArch32 Execution States

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

ARM64 processors can operate in two **Execution States**:
- **AArch64** — the 64-bit execution state, using A64 instruction set.
- **AArch32** — the 32-bit execution state, using A32 (ARM) or T32 (Thumb) instruction sets, backward compatible with ARMv7.

The execution state determines:
- Register widths (64-bit vs 32-bit).
- Available instruction sets.
- Exception model and system register access mechanism.
- Page table format (64-bit descriptors vs 32-bit).

An ARMv8/ARMv9 processor always boots in AArch64 at the highest implemented EL. Lower ELs can be configured for AArch32 via the `HCR_EL2.RW` and `SCR_EL3.RW` bits.

---

## 2. Execution State Determination per EL

The execution state of each EL is determined by the bit `RW` in the controlling register of the EL above:

```
SCR_EL3.RW:
  0 = EL2 is AArch32
  1 = EL2 is AArch64

HCR_EL2.RW:
  0 = EL1 and EL0 are AArch32 (unless overridden)
  1 = EL1 is AArch64

SPSR_ELx.M[4]:
  0 = Exception was taken from AArch64 state
  1 = Exception was taken from AArch32 state
```

A crucial rule: **a higher EL cannot be AArch32 if a lower EL is AArch64**. The states must be ordered such that AArch64 is at the top and AArch32 at the bottom, with no "holes."

Valid configurations:
```
AArch64 EL3 / AArch64 EL2 / AArch64 EL1 / AArch64 EL0  ← All 64-bit (typical Linux)
AArch64 EL3 / AArch64 EL2 / AArch64 EL1 / AArch32 EL0  ← 32-bit apps on 64-bit kernel
AArch64 EL3 / AArch64 EL2 / AArch32 EL1 / AArch32 EL0  ← 32-bit guest OS under 64-bit hypervisor
AArch64 EL3 / AArch32 EL2 / AArch32 EL1 / AArch32 EL0  ← Rare: 32-bit hypervisor
```

---

## 3. AArch64 Register File

```
General Purpose:  X0–X30 (64-bit), W0–W30 (lower 32-bit of same registers)
Special Purpose:
  XZR / WZR    — Zero register (reads as 0, writes discarded)
  SP           — Stack pointer (alias for SP_EL0 or SP_ELx depending on SPSel)
  PC           — Program Counter (not directly addressable as GPR)
  LR           — Link Register = X30

SIMD/FP:        V0–V31 (128-bit), B/H/S/D/Q views
SVE:            Z0–Z31 (scalable width: 128–2048 bits), P0–P15 (predicates)

System:         PSTATE (N,Z,C,V,D,A,I,F,SS,IL,EL,nRW,SP,PAN,UAO,BTYPE,SSBS,TCO)
```

**Key difference from AArch32**: In AArch64, `X0`–`X30` are always 64-bit. Writing to `W0` (32-bit form) **zero-extends** to `X0`. This eliminates the "upper half garbage" problem of AArch32's mixed-width registers.

---

## 4. AArch32 Register File (for comparison)

```
General Purpose:  R0–R12 (32-bit), R13 (SP), R14 (LR), R15 (PC)
Banked (mode-dependent): SP_usr, SP_svc, SP_abt, SP_irq, SP_fiq, SP_und
CPSR:            Current Program Status Register (N,Z,C,V,I,F,T,M[4:0])
SPSR:            Saved Program Status Register (banked per mode)
SIMD/FP:         D0–D31, S0–S31, Q0–Q15
```

**Key differences from AArch64**:
1. `R15` (PC) is a general-purpose register in AArch32 — you can read/write it like any GPR. Writing PC causes a branch. This leads to complex pipeline interactions.
2. Register banking: Different processor modes (SVC, IRQ, FIQ, ABT, UND, USR, SYS) have different physical registers behind some logical registers. FIQ mode has its own R8–R12.
3. Condition codes on almost every instruction (e.g., `ADDEQ R0, R1, R2`).

---

## 5. Instruction Set Differences

| Feature | AArch64 (A64) | AArch32 (A32/T32) |
|---|---|---|
| Instruction width | Fixed 32-bit | A32: 32-bit; T32 (Thumb): 16/32-bit mixed |
| Condition codes | Only B-type branches | Nearly all instructions |
| PC accessibility | Not a GPR | R15 = PC, directly readable/writable |
| Load/Store | LDR, STR, LDP, STP | LDR, STR, LDM, STM |
| Atomics | LDXR/STXR, CAS (LSE) | LDREX/STREX |
| SIMD | NEON/SVE integrated | NEON optional coprocessor |
| System regs | MSR/MRS with symbolic names | MCR/MRC with CP15 encoding |

---

## 6. Exception Model Differences

### AArch64 Exception Model
- 4 Exception Levels (EL0–EL3).
- `VBAR_ELx` points to 16-entry vector table (offset-based).
- Exceptions always taken in AArch64 state at the target EL.
- `SPSR_ELx` saves the interrupted state.
- Separate `ELR_ELx` per level.

### AArch32 Exception Model
- 7 processor modes: USR, FIQ, IRQ, SVC, MON, ABT, UND, SYS, HYP.
- `VBAR` for high vectors (optional, or use `0x00000000`/`0xFFFF0000`).
- `CPSR.M[4:0]` field selects the mode.
- Mode-banked registers (SP, LR, SPSR per mode).

### Taking Exceptions from AArch32 EL0 to AArch64 EL1

When a 32-bit user process calls `SVC`:
1. Exception taken to EL1 (AArch64 kernel).
2. `SPSR_EL1.M[4]=1` (was AArch32), `SPSR_EL1.M[3:0]` encodes AArch32 mode.
3. `SPSR_EL1.T` indicates whether it was Thumb or ARM state.
4. Linux handles this at `VBAR_EL1 + 0x600` (Lower EL AArch32 synchronous).
5. The 32-bit syscall number is read from `W8` (Thumb `SVC`) or from the instruction encoding.

---

## 7. Linux Kernel Support for AArch32

Linux on ARM64 supports 32-bit AArch32 user processes through `CONFIG_COMPAT`:

```c
// arch/arm64/kernel/entry.S
// 0x600 offset: lower EL, AArch32 synchronous
SYM_CODE_START(vectors)
    + 0x600: el0_sync_compat   // 32-bit SVC handler

// arch/arm64/kernel/sys_compat.c
// 32-bit syscall table mapping
asmlinkage long compat_sys_mmap2(...)
```

Key `compat_task` handling:
```c
// include/linux/compat.h
#ifdef CONFIG_COMPAT
static inline bool is_compat_task(void)
{
    return test_thread_flag(TIF_32BIT);
}
#endif
```

When creating an AArch32 process:
- `TIF_32BIT` flag set in `thread_info`.
- Separate signal delivery path (`compat_setup_rt_frame()`).
- 32-bit VDSO mapped (providing 32-bit `sigreturn` trampoline).
- `TTBR0_EL1` points to a 32-bit-compatible page table (3-level with `T0SZ=32`).

---

## 8. Interprocessing (Changing Execution State)

In AArch64, **you cannot directly branch between AArch64 and AArch32 code** within the same EL. The execution state change only happens on exception return (`ERET`) or exception entry.

To spawn a 32-bit process from a 64-bit kernel:
1. `execve()` loads a 32-bit ELF binary.
2. Kernel sets up `SPSR_EL1.M[4]=1`, `SPSR_EL1.M[3:0]=0` (USR mode), `SPSR_EL1.T` per entry point.
3. `ERET` drops to AArch32 EL0.
4. `HCR_EL2.TGE=0`, `HCR_EL2.RW=1` (EL1 is AArch64), EL0 can be AArch32.

---

## 9. System Register Access Differences

### AArch64 — MSR/MRS
```asm
// Read SCTLR_EL1
mrs x0, SCTLR_EL1

// Write TCR_EL1
msr TCR_EL1, x1

// Registers named symbolically (assembler resolves to encoding)
```

### AArch32 — MCR/MRC via CP15
```asm
// Read SCTLR (equivalent of SCTLR_EL1)
MRC p15, 0, r0, c1, c0, 0    // Read CP15 c1,c0,0 → r0

// Write TTBR0
MCR p15, 0, r1, c2, c0, 0    // Write r1 → CP15 c2,c0,0
```

The AArch32 coprocessor encoding `MRC/MCR p15, opc1, Rd, CRn, CRm, opc2` maps to AArch64 `MSR/MRS` with symbolic register names. A mapping table exists in the ARM Architecture Reference Manual.

---

## 10. Interview Questions & Answers

**Q1: Can a 64-bit Linux kernel run 32-bit applications on ARM64?**

Yes, via the Compat ABI (`CONFIG_COMPAT`). The kernel runs at EL1 in AArch64, but when executing a 32-bit ELF, it sets `SPSR_EL1` to indicate AArch32 USR mode and uses `ERET` to enter user space in AArch32 state. The kernel handles the different syscall conventions, register layouts (r0–r7 for parameters), and VDSO for the 32-bit case.

**Q2: What prevents AArch32 code at EL0 from accessing EL1 resources?**

The exception model: any privileged instruction from AArch32 EL0 (e.g., `MCR p15`) causes an Undefined Instruction exception taken to AArch64 EL1, since `HCR_EL2.RW=1` means EL1 is AArch64. The Linux kernel's EL1 handler sends `SIGILL` to the process.

**Q3: Is PC a general-purpose register in AArch64?**

No. Unlike AArch32 where R15=PC is a GPR, in AArch64 the PC is not in the general-purpose register file. It cannot be read/written directly as a source/destination operand in data processing instructions. You can read the PC using `ADR Xd, .` (PC-relative address) or `ADRP`.

**Q4: What is the nRW bit in PSTATE/SPSR?**

`nRW` (not Register Width) indicates execution state: `0 = AArch64`, `1 = AArch32`. In `SPSR_ELx`, this is the `M[4]` bit. When the kernel returns from an exception to a 32-bit process, it sets `SPSR_EL1.M[4]=1`.

**Q5: How does ARM64 handle AArch32 FIQ banked registers?**

In AArch32, FIQ mode has banked registers R8–R14. In AArch64, when EL0 is AArch32 and a FIQ is taken to EL1 (AArch64), the hardware saves/restores the AArch32 banked registers transparently. The AArch64 EL1 exception handler does not see them as X8–X14; they are only visible when the CPU is executing in AArch32 state.

---

## 11. Quick Reference

| Property | AArch64 | AArch32 |
|---|---|---|
| GPR count | 31 (X0–X30) + XZR | 16 (R0–R15, includes SP, LR, PC) |
| GPR width | 64-bit (W-form = lower 32) | 32-bit |
| PC as GPR | No | Yes (R15) |
| SIMD registers | V0–V31 (128-bit), SVE | D0–D31, Q0–Q15 |
| System reg access | MSR/MRS (symbolic) | MCR/MRC (CP15 encoding) |
| Condition codes | Branch-only | Nearly universal |
| Instruction width | Fixed 32-bit | A32: 32-bit; T32: 16 or 32-bit |
| Stack alignment | 16-byte mandatory | 8-byte AAPCS recommended |
| Exception levels | EL0–EL3 | Processor modes (USR/SVC/IRQ/FIQ/...) |
| SPSR | Banked per EL | Banked per mode |
