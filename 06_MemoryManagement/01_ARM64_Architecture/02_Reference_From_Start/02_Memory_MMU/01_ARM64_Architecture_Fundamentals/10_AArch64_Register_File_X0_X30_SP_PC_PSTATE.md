# AArch64 Register File: X0–X30, SP, PC, PSTATE

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, NVIDIA, Qualcomm, AMD

---

## 1. Concept Foundation

The AArch64 register file is the foundational state of every CPU thread. Understanding it deeply matters for:
- Calling convention compliance (correct parameter passing)
- Exception handling (which registers hardware saves)
- Compiler code generation (register allocation strategy)
- Security (Pointer Authentication uses X17/X30)
- Debugging (reading crash dumps, ftrace, perf)

AArch64 has a completely redesigned register file compared to AArch32:
- 31 general-purpose 64-bit registers (X0–X30), vs 16 in ARM32
- 32 SIMD/FP registers Q0–Q31 (128-bit each), separate bank
- Dedicated zero register (XZR/WZR)
- No general-purpose banked registers per mode (unlike ARM32)
- PSTATE replaces CPSR (no direct CPSR equivalent instruction)

---

## 2. General Purpose Register File

### X0–X30: 64-bit General Purpose Registers

Each Xi register has:
- **Xi** — full 64-bit access
- **Wi** — lower 32-bit access (writing Wi zero-extends to Xi; upper 32 bits become 0)

```
┌─────────────────────────────────────────────────────────┐
│ X0                                                 [63:0] │
│         ┌─────────────────────────┐                       │
│         │ W0                [31:0] │                       │
└─────────────────────────────────────────────────────────┘
```

Accessing W0 always clears bits [63:32] of X0. There is no 8-bit or 16-bit named sub-register in AArch64 (unlike x86's AL/AH/AX).

---

## 3. AAPCS64 Calling Convention — Register Roles

The ARM Architecture Procedure Call Standard for AArch64 (AAPCS64) defines:

```
Parameter / Result Registers (caller-saved):
  X0–X7    — Function arguments (1st–8th param) and return values
               X0     = 1st argument / integer return value
               X1     = 2nd argument / 2nd return value (if composite)
               X2–X7  = 3rd–8th arguments

Caller-saved (scratch) registers:
  X9–X15   — Temporary registers (caller must save if it needs them after call)
  X16(IP0) — Intra-procedure call scratch register 1 (used by linker veneers, PLT)
  X17(IP1) — Intra-procedure call scratch register 2 (also used by PAC)
  X18      — Platform register (reserved; ShadowCallStack pointer on some platforms)

Callee-saved (preserved) registers:
  X19–X28  — Callee must save and restore if used
  X29(FP)  — Frame pointer (callee-saved; points to saved FP/LR pair on stack)
  X30(LR)  — Link register (return address; callee-saved in leaf functions)

Special registers:
  XZR/WZR  — Zero register (reads always return 0; writes are no-ops)
  SP       — Stack pointer (aliased to SP_EL1 or SP_EL0 per SPSel)
  PC       — Program counter (not directly readable via MOV; use ADR/ADRP)
```

### AAPCS64 Stack Frame Layout

```
High address
┌─────────────────────────┐
│  Caller's frame         │
├─────────────────────────┤  ← FP (X29) points here
│  Saved FP (X29)         │  [FP, #0]
│  Saved LR (X30)         │  [FP, #8]
├─────────────────────────┤
│  Callee-saved regs      │  X19–X28 as needed
│  (X19, X20, ...)        │
├─────────────────────────┤
│  Local variables        │
│  Spill area             │
├─────────────────────────┤  ← SP (stack pointer)
Low address
```

Example function prologue/epilogue:
```asm
my_function:
    stp     x29, x30, [sp, #-16]!   // Save FP+LR, decrement SP
    mov     x29, sp                  // Set FP = current SP
    stp     x19, x20, [sp, #-16]!   // Save callee-saved regs (if used)
    // ... function body
    ldp     x19, x20, [sp], #16     // Restore callee-saved regs
    ldp     x29, x30, [sp], #16     // Restore FP+LR, increment SP
    ret                              // Jump to X30 (LR)
```

---

## 4. Special Registers

### XZR / WZR — Zero Register

- **Read**: Always returns 0. `ADD X0, X1, XZR` → X0 = X1 + 0.
- **Write**: Discards result. `STR XZR, [X0]` → stores 64-bit zero to memory at X0.
- Register encoding: XZR = register 31 in most instruction encodings.
- Exception: In stack-pointer context, encoding 31 means SP, not XZR. The distinction is instruction-specific.

```asm
// Use XZR for conditional zero-fill:
str     xzr, [x0, #0]    // *x0 = 0 (64-bit zero store)
str     wzr, [x0, #0]    // *x0 = 0 (32-bit zero store)

// Use XZR for comparison:
cmp     x0, xzr          // Same as CMP X0, #0 but shorter encoding
cbz     x0, label        // Compare and Branch if Zero (dedicated instruction)
```

### SP — Stack Pointer

- Not a general-purpose register in AArch64. Cannot be used as a source in most instructions.
- At EL1, `SP` is either `SP_EL1` (SPSel=1, handler mode) or `SP_EL0` (SPSel=0, thread mode).
- **Alignment**: SP must be 16-byte aligned before any function call that might throw exceptions (ABI requirement). Misaligned SP with `SCTLR_EL1.SA=1` causes a Stack Alignment Fault.
- To read/write SP: `MOV X0, SP` or `MOV SP, X0` (special encoding).

### PC — Program Counter

- Not directly readable in AArch64 as a general-purpose register.
- Use `ADR X0, .` to get current PC value into a general register.
- Use `ADRP X0, label` to get page-aligned PC-relative address.
- On exception, hardware saves PC to `ELR_ELx`, not to a general register.

---

## 5. PSTATE — Processor State

`PSTATE` in AArch64 is not a single register. It's a collection of fields from different sources, accessible collectively via `SPSR_ELx` (when saved on exception) or individually via `MSR/MRS` instructions.

### PSTATE Field Breakdown

```
PSTATE bits in SPSR_ELx format (when exception taken to EL1):

Bit[31]  N     — Negative flag (result MSB was 1)
Bit[30]  Z     — Zero flag (result was 0)
Bit[29]  C     — Carry flag (unsigned overflow / borrow)
Bit[28]  V     — Overflow flag (signed overflow)

Bit[27]  Q     — Cumulative saturation flag (SIMD/USAT/SSAT)
Bit[26:25] IT  — If-Then state bits (AArch32 compatibility, RES0 in AArch64)
Bit[24]  DIT   — Data Independent Timing (ARMv8.4, prevents timing side-channels)
Bit[23]  SSBS  — Speculative Store Bypass Safe (ARMv8.5 Spectre mitigation)
Bit[22]  PAN   — Privileged Access Never (ARMv8.1: prevents EL1 from accessing user VAs)
Bit[21]  UAO   — User Access Override (ARMv8.2: LDTR/STTR behave like LDR/STR)
Bit[20]  TCO   — Tag Check Override (ARMv8.5 MTE: disable tag checks)
Bit[19:16] — GE — SIMD Greater-than-or-Equal flags (AArch32, RES0 in AArch64)

Bit[15:10] — RES0

Bit[9]   D     — Debug exception mask (1=masked)
Bit[8]   A     — SError (Asynchronous abort) mask (1=masked)
Bit[7]   I     — IRQ mask (1=masked, interrupts disabled)
Bit[6]   F     — FIQ mask (1=masked)

Bit[5]   — RES0
Bit[4]   M[4]  — nRW: Execution state (0=AArch64, 1=AArch32)

Bit[3:0] M[3:0] — Mode field:
  AArch64:
    0b0000 = EL0t (EL0 with SP_EL0)
    0b0100 = EL1t (EL1 with SP_EL0)
    0b0101 = EL1h (EL1 with SP_EL1)
    0b1000 = EL2t (EL2 with SP_EL0)
    0b1001 = EL2h (EL2 with SP_EL2)
    0b1100 = EL3t (EL3 with SP_EL0)
    0b1101 = EL3h (EL3 with SP_EL3)
```

### Accessing PSTATE Fields Directly

```asm
// Read individual PSTATE fields:
mrs x0, nzcv        // Read N,Z,C,V flags (bits [31:28])
mrs x0, daif        // Read D,A,I,F interrupt masks
mrs x0, currentel   // Read current EL (bits [3:2])
mrs x0, spsel       // Read SPSel (0=SP_EL0, 1=SP_ELx)

// Write PSTATE fields:
msr daifset, #2     // Set I bit (disable IRQs): immediate 4-bit mask
msr daifclr, #2     // Clear I bit (enable IRQs)
msr nzcv, x0        // Set N,Z,C,V flags from X0
msr pan, #1         // Enable PAN (ARMv8.1)
msr uao, #0         // Disable UAO
```

### DAIF — Interrupt Masking

```c
// Linux kernel uses:
local_irq_disable():  MSR DAIFSET, #2   // Set I bit
local_irq_enable():   MSR DAIFCLR, #2   // Clear I bit
local_fiq_disable():  MSR DAIFSET, #1   // Set F bit
local_irq_save(flags): MRS x0, DAIF → save; MSR DAIFSET, #2
local_irq_restore(flags): MSR DAIF, x0 → restore

// arch/arm64/include/asm/irqflags.h
static inline void arch_local_irq_enable(void)
{
    asm volatile("msr daifclr, #2" : : : "memory");
}
static inline void arch_local_irq_disable(void)
{
    asm volatile("msr daifset, #2" : : : "memory");
}
```

---

## 6. Pointer Authentication (PAC) and X17/LR

ARMv8.3 introduces Pointer Authentication. LR (X30) and return addresses are signed with a cryptographic MAC:

```asm
// In function prologue (with PAC enabled):
paciasp        // Sign LR using SP as context, key IA
               // Modifies upper bits of X30 with PAC

// In function epilogue:
autiasp        // Authenticate LR using SP as context
               // If PAC invalid (corrupted return address): fault
ret            // Return via X30
```

The signing key is stored in system registers (`APIAKeyLo_EL1`, `APIAKeyHi_EL1`) that only the kernel can write. X16/X17 are used by PLT stubs and PAC veneer code for indirect calls.

---

## 7. Linux Kernel: struct pt_regs

When a Synchronous Exception (syscall, abort, etc.) is taken to EL1, the kernel's `kernel_entry` macro saves all registers to `struct pt_regs`:

```c
// arch/arm64/include/asm/ptrace.h
struct pt_regs {
    union {
        struct user_pt_regs user_regs;  // X0–X30, SP, PC, PSTATE
        struct {
            u64 regs[31];   // X0–X30
            u64 sp;         // Stack pointer
            u64 pc;         // Program counter
            u64 pstate;     // PSTATE (from SPSR_EL1)
        };
    };
    u64 orig_x0;    // Original X0 (syscall number / first arg)
    s32 syscallno;  // Syscall number
    u32 unused2;
    u64 sdei_ttbr1; // SDEI context TTBR1
    u64 pmr_save;   // GIC PMR save
    u64 stackframe[2]; // Unused entry padding
    u64 lockdep_hardirqs;
    u64 exit_rcu;
};
```

### kernel_entry Macro (entry.S)

```asm
// arch/arm64/kernel/entry.S
.macro kernel_entry, el, regsize = 64
    sub     sp, sp, #PT_REGS_SIZE   // Allocate struct pt_regs on stack
    stp     x0, x1, [sp, #16 * 0]  // Save X0, X1
    stp     x2, x3, [sp, #16 * 1]  // Save X2, X3
    stp     x4, x5, [sp, #16 * 2]  // ...
    stp     x6, x7, [sp, #16 * 3]
    stp     x8, x9, [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    
    // Save SP, PC, PSTATE from hardware-banked registers
    mrs     x21, sp_el0              // User SP (if coming from EL0)
    mrs     x22, elr_el1             // Return PC
    mrs     x23, spsr_el1            // Saved PSTATE
    stp     x30, x21, [sp, #PT_REGS_X30]  // X30 (LR) and user SP
    stp     x22, x23, [sp, #PT_REGS_PC]   // PC and PSTATE
.endm
```

---

## 8. Interview Questions & Answers

**Q1: What is the difference between X0 and W0, and what happens when you write W0?**

`X0` is the full 64-bit register. `W0` is the lower 32 bits of `X0`. When you write to `W0` (e.g., `MOV W0, #5`), the upper 32 bits [63:32] of `X0` are zero-extended to 0. This is different from ARM32 where writing a partial register left upper bits unchanged. In AArch64, this zero-extension is guaranteed, making it safe to use 32-bit operations and then read the result as 64-bit.

**Q2: How does Linux disable interrupts on ARM64?**

`local_irq_disable()` executes `MSR DAIFSET, #2` which sets the I-bit (IRQ mask) in PSTATE. `local_irq_enable()` executes `MSR DAIFCLR, #2` to clear it. The `#2` is a 4-bit immediate where bit 1 = I (IRQ), bit 0 = F (FIQ), bit 2 = A (SError), bit 3 = D (Debug). These directly modify the named PSTATE fields.

**Q3: How do you read the current EL from code?**

```asm
mrs x0, currentel    // Reads PSTATE.EL into x0
lsr x0, x0, #2      // Bits [3:2] are EL; shift right to get 0/1/2/3
```
The `CurrentEL` pseudo-register encodes the EL in bits [3:2].

**Q4: What is PAN (Privileged Access Never) and why is it important?**

`PAN=1` in PSTATE prevents the kernel (EL1) from directly accessing user-space virtual addresses. Any attempt by EL1 code to load/store via a user-space VA with PAN enabled causes a Permission Fault. This is a security feature: it prevents certain kernel exploits (like ret2usr attacks) where attacker-controlled data at a user-space address is accessed by the kernel. Linux sets PAN when entering the kernel and uses `ldtr`/`sttr` (unprivileged load/store) instructions explicitly for user access via `copy_to_user()` / `copy_from_user()`.

**Q5: Why does AArch64 have no direct PC read instruction?**

In ARM32, `MOV R0, PC` reads the PC. In AArch64, PC is not a general-purpose register and cannot appear in `MOV`. Instead, `ADR X0, .` computes an address relative to PC (current instruction address + 0 = PC) into X0. This was a deliberate design choice: direct PC reads were often used for position-independent code, and AArch64 provides `ADRP`/`ADR` for this purpose which are safer (no pipeline PC offset confusion).

---

## 9. Quick Reference

### Register Role Summary

| Registers | AAPCS64 Role | Save Responsibility |
|---|---|---|
| X0–X7 | Args/return values | Caller |
| X8 | Indirect result location / XR | Caller |
| X9–X15 | Temporary/scratch | Caller |
| X16 (IP0) | Intra-call scratch / PLT | Caller |
| X17 (IP1) | Intra-call scratch / PAC | Caller |
| X18 | Platform register | Platform-defined |
| X19–X28 | Callee-saved | Callee |
| X29 (FP) | Frame pointer | Callee |
| X30 (LR) | Link register (return addr) | Callee |
| XZR | Zero register | N/A (hardware) |
| SP | Stack pointer | Managed explicitly |
| PC | Program counter | Hardware (ELR on exception) |

### PSTATE Key Flags

| Flag | Bit | Purpose |
|---|---|---|
| N | 31 | Negative result |
| Z | 30 | Zero result |
| C | 29 | Carry / unsigned overflow |
| V | 28 | Signed overflow |
| D | 9 | Debug mask |
| A | 8 | SError mask |
| I | 7 | IRQ mask |
| F | 6 | FIQ mask |
| PAN | 22 | Privileged Access Never |
| UAO | 23 | User Access Override |
| SSBS | 12 | Spectre Store Bypass Safe |
| TCO | 25 | MTE Tag Check Override |
