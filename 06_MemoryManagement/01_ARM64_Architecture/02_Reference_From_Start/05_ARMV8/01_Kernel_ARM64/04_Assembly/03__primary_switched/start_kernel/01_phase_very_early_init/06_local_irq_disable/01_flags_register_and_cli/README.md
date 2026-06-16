# x86 RFLAGS Register and Interrupt Control

## RFLAGS Register Layout (x86-64)

```
Bit 63-22: Reserved
Bit 21: ID    — CPUID instruction supported (if modifiable)
Bit 20: VIP   — Virtual Interrupt Pending (VMx)
Bit 19: VIF   — Virtual Interrupt Flag (VMx)
Bit 18: AC    — Alignment Check
Bit 17: VM    — Virtual 8086 Mode
Bit 16: RF    — Resume Flag (debug)
Bit 14: NT    — Nested Task
Bit 13-12: IOPL — I/O Privilege Level (0=kernel, 3=user)
Bit 11: OF    — Overflow Flag
Bit 10: DF    — Direction Flag (string ops)
Bit 9:  IF ◄── Interrupt Enable Flag  ← CLI/STI control THIS bit
Bit 8:  TF    — Trap Flag (single-step debugging)
Bit 7:  SF    — Sign Flag
Bit 6:  ZF    — Zero Flag
Bit 4:  AF    — Auxiliary Carry
Bit 2:  PF    — Parity Flag
Bit 0:  CF    — Carry Flag
```

## CLI and STI Instructions

| Instruction | Effect | Privilege |
|-------------|--------|-----------|
| `CLI` | Clear IF=0, disable maskable interrupts | CPL=0 (kernel only) |
| `STI` | Set IF=1, enable maskable interrupts | CPL=0 (kernel only) |
| `PUSHF` | Push RFLAGS to stack | Any |
| `POPF` | Pop RFLAGS from stack, restores IF | CPL=0 for IF bit |

If userspace tries `CLI`, it gets `#GP` (General Protection Fault).

## Save/Restore Pattern

```c
// Saving IRQ state for spinlocks:
unsigned long flags;

local_irq_save(flags);      // PUSHF → pop to flags (saves IF bit)
// critical section
local_irq_restore(flags);   // push flags → POPF (restores IF bit)
```

This pattern is used in `spin_lock_irqsave()` to safely use spinlocks in interrupt context — if IRQs were already disabled when we entered, they stay disabled after we leave.

## ARM64 DAIF Register

ARM64 uses the DAIF system register to control exceptions:
```
D = Debug exceptions masked
A = SError (Asynchronous abort) masked
I = IRQ masked  ← equivalent to x86 CLI when set
F = FIQ masked  ← Fast Interrupt Queue
```

```
msr daifset, #2    // set I bit = mask IRQs (equivalent to CLI)
msr daifclr, #2    // clear I bit = unmask IRQs (equivalent to STI)
```

## Interview Q&A

### Q1: Can a userspace process disable interrupts?
**A:** No. `CLI` is a privileged instruction (requires CPL=0 on x86). User processes running at CPL=3 that execute `CLI` receive a `#GP` (General Protection Fault). On ARM64, the DAIF register bits can only be modified in EL1 (kernel) and above, not EL0 (user). However, some hypervisor configurations (VT-x VMX, KVM) give VMs the illusion of interrupt control via Virtual Interrupt Flag (VIF bit in RFLAGS) — the CLI in a guest only sets VIF, not real IF, so the host isn't affected.

### Q2: What is the IOPL field in RFLAGS and when is it used?
**A:** IOPL (I/O Privilege Level, bits 13-12) controls whether `IN`, `OUT`, `INS`, `OUTS` x86 port I/O instructions can be used by userspace. With IOPL=3, user processes can do direct port I/O (used by some legacy X11 display drivers, DOSBox emulator). The `iopl()` and `ioperm()` syscalls change this. Modern kernels/hardware use memory-mapped I/O instead, making direct port I/O obsolete. Android kernel (Qualcomm) never uses IOPL — all I/O is MMIO or DMA.
