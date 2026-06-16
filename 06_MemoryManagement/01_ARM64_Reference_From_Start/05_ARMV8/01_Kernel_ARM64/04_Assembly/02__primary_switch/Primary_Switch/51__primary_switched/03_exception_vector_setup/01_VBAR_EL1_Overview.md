# Exception Vector Setup — Overview

## The Three Assembly Lines

```asm
adr_l   x8, vectors        // x8 = VA of exception vector table
msr     vbar_el1, x8       // VBAR_EL1 = vector table base address
isb                        // sync: VBAR_EL1 change takes effect immediately
```

These three instructions install the kernel's exception vector table. Until this
runs, any exception (IRQ, fault, SVC, etc.) will jump to whatever address firmware
left in `VBAR_EL1` — which is either a firmware handler or garbage.

---

## What `VBAR_EL1` Is

`VBAR_EL1` = **Vector Base Address Register EL1**. It holds the base address of the
exception vector table for EL1 (the kernel). The ARM64 architecture defines a fixed
table layout at specific offsets from this base:

```
VBAR_EL1 + 0x000:  Current EL, SP_EL0:  Synchronous
VBAR_EL1 + 0x080:  Current EL, SP_EL0:  IRQ/vIRQ
VBAR_EL1 + 0x100:  Current EL, SP_EL0:  FIQ/vFIQ
VBAR_EL1 + 0x180:  Current EL, SP_EL0:  SError/vSError

VBAR_EL1 + 0x200:  Current EL, SP_ELx:  Synchronous  ← Linux uses this bank
VBAR_EL1 + 0x280:  Current EL, SP_ELx:  IRQ/vIRQ     ← Linux uses this bank
VBAR_EL1 + 0x300:  Current EL, SP_ELx:  FIQ/vFIQ
VBAR_EL1 + 0x380:  Current EL, SP_ELx:  SError

VBAR_EL1 + 0x400:  Lower EL, AArch64:   Synchronous  ← user syscalls/faults
VBAR_EL1 + 0x480:  Lower EL, AArch64:   IRQ
VBAR_EL1 + 0x500:  Lower EL, AArch64:   FIQ
VBAR_EL1 + 0x580:  Lower EL, AArch64:   SError

VBAR_EL1 + 0x600:  Lower EL, AArch32:   Synchronous
VBAR_EL1 + 0x680:  Lower EL, AArch32:   IRQ
VBAR_EL1 + 0x700:  Lower EL, AArch32:   FIQ
VBAR_EL1 + 0x780:  Lower EL, AArch32:   SError
```

Each entry is 128 bytes (32 instructions) of actual code — not a pointer, but
direct code at that address.

---

## `vectors` — The Linux Exception Vector Table

```c
// arch/arm64/kernel/entry.S
    .align  11       // 2048-byte alignment (VBAR_EL1 requires 11-bit alignment)
SYM_CODE_START(vectors)
    kernel_ventry   1, t, 64, sync      // VBAR+0x000: current EL, SP0, sync
    kernel_ventry   1, t, 64, irq       // VBAR+0x080: current EL, SP0, irq
    kernel_ventry   1, t, 64, fiq       // VBAR+0x100: current EL, SP0, fiq
    kernel_ventry   1, t, 64, error     // VBAR+0x180: current EL, SP0, serror
    kernel_ventry   1, h, 64, sync      // VBAR+0x200: current EL, SPx, sync
    kernel_ventry   1, h, 64, irq       // VBAR+0x280: current EL, SPx, irq
    kernel_ventry   1, h, 64, fiq       // VBAR+0x300: current EL, SPx, fiq
    kernel_ventry   1, h, 64, error     // VBAR+0x380: current EL, SPx, serror
    kernel_ventry   0, t, 64, sync      // VBAR+0x400: lower EL, AArch64, sync
    kernel_ventry   0, t, 64, irq       // VBAR+0x480: lower EL, AArch64, irq
    kernel_ventry   0, t, 64, fiq       // VBAR+0x500: lower EL, AArch64, fiq
    kernel_ventry   0, t, 64, error     // VBAR+0x580: lower EL, AArch64, serror
    kernel_ventry   0, t, 32, sync      // VBAR+0x600: lower EL, AArch32, sync
    ...
SYM_CODE_END(vectors)
```

---

## The Ordering Constraint: After `init_cpu_task`, Before C Code

```asm
init_cpu_task x4, x5, x6    // sets up stack, current, per-CPU
adr_l   x8, vectors         // VBAR setup begins
msr     vbar_el1, x8
isb                          // VBAR setup complete
stp     x29, x30, [sp, #-16]!  // first C stack frame setup
mov     x29, sp
...
bl      start_kernel         // first C call
```

VBAR must be set AFTER `init_cpu_task` because:
- Exception handlers call C code (e.g., `do_el1_irq`)
- C code requires valid `sp`, `sp_el0`, and `tpidr_el1`
- If an exception arrived before `init_cpu_task` and vectored to the new handlers,
  the handlers would crash trying to use `current` or `this_cpu_read()`

VBAR must be set BEFORE `start_kernel` because:
- `start_kernel` enables local interrupts (`local_irq_enable()`)
- With interrupts enabled, IRQs can arrive at any moment
- The IRQ vector (VBAR+0x280) must be valid before enabling IRQs

---

## Why `isb` After `msr vbar_el1`?

`isb` (Instruction Synchronization Barrier) flushes the instruction pipeline.
After writing to a system register like `VBAR_EL1`, the new value is not
necessarily visible to subsequent instruction fetches without `isb`.

Without `isb`:
- The CPU might speculatively fetch/execute the next instruction using the OLD
  `VBAR_EL1` value if an exception occurred during the pipeline hazard window
- The behavior is UNPREDICTABLE (per ARM Architecture Reference Manual)

With `isb`:
- Guarantees all subsequent exceptions will use the NEW `VBAR_EL1` value
- Required by the ARM architecture after any `msr` that changes exception-related state

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
VBAR_EL1 (Vector Base Address Register, EL1) holds the base address of the EL1 exception vector table. When an exception is taken to EL1 (IRQ, FIQ, SError, Synchronous abort), the CPU computes the vector offset from the exception type and PSTATE.SP, adds it to VBAR_EL1, and jumps to the resulting address (this is the hardware branch, not a software branch). The vector table has 16 entries (4 types x 4 SP variants) each spaced 0x80 bytes apart. VBAR_EL1 must be aligned to a 2 KB boundary. With KASLR, VBAR_EL1 is randomized as part of the kernel image.

### Kernel Perspective (Linux ARM64)
Linux sets VBAR_EL1 in __primary_switched (arch/arm64/kernel/head.S) using:
  adr_l  x8, vectors          // load VA of the vectors table
  msr    vbar_el1, x8         // write to VBAR_EL1
  isb                          // synchronize
This is done after the MMU is enabled and the kernel VA is active. The 'vectors' symbol is defined in arch/arm64/kernel/entry.S and maps the 16 exception handlers. Until this point, any exception would take the CPU to whatever garbage is at VBAR_EL1 (undefined on reset), so the early boot path must not trigger any exceptions.

### Memory Perspective (ARMv8 Memory Model)
VBAR_EL1 stores a VA (in the kernel text mapping). The exception vector table at that VA is Normal Inner-Shareable Read-Only (from an architectural view; the Linux kernel maps it as read-only/execute). When an exception fires, the CPU reads VBAR_EL1, computes the target VA, and fetches the instruction at that VA via the TLB/I-cache. The vector table is part of the kernel text and benefits from I-cache warming; exception entry latency is minimized once the I-cache has been populated with the vector code.