# ARM64 Exception Vector Table Architecture

## The Fixed-Offset Table

ARM64 does NOT use a table of function pointers. Instead, the VBAR points to an
area of EXECUTABLE CODE where each 128-byte "slot" contains actual instructions.
When an exception occurs, the CPU directly jumps to `VBAR + offset` and begins
executing the code there.

```
VBAR_EL1 ──► [128 bytes of code: kernel-to-kernel sync handler]    +0x000
             [128 bytes of code: kernel-to-kernel IRQ handler]      +0x080
             [128 bytes of code: kernel-to-kernel FIQ handler]      +0x100
             [128 bytes of code: kernel-to-kernel SError handler]   +0x180
             [128 bytes of code: kernel-to-kernel sync (SPx) ]      +0x200
             [128 bytes of code: kernel-to-kernel IRQ (SPx)  ]      +0x280
             ...
             [128 bytes of code: user-to-kernel sync handler ]      +0x400
             [128 bytes of code: user-to-kernel IRQ handler  ]      +0x480
             ...
```

Each 128-byte slot is 32 × 4-byte instructions. Most entry handlers don't fit
in 32 instructions — they branch to the real handler code immediately:

```asm
// VBAR+0x400 (user AArch64 sync: typically syscall)
kernel_ventry:
    // save regs, switch to kernel stack, check for KPTI
    b   el0t_64_sync    // jump to real 64-bit sync handler (fits in 128 bytes)
```

---

## The `kernel_ventry` Macro

```asm
// arch/arm64/kernel/entry.S
.macro kernel_ventry, el:req, ht:req, regsize:req, label:req
    .align 7    // each entry is 128 bytes (2^7)
    .Lventry_start\@:
    .if \el == 0    // from EL0 (user space)
        // KPTI: switch page tables
        .if \regsize == 64
            mov     x0, xzr
            b       el\el\ht\()_\regsize\()_\label    // branch to real handler
        .endif
    .else           // from EL1 (kernel space)
        sub     sp, sp, #PT_REGS_SIZE   // reserve pt_regs on kernel stack
        b       el\el\ht\()_\regsize\()_\label        // branch to real handler
    .endif
    .org .Lventry_start\@ + 0x80        // enforce 128-byte entry size
.endm
```

---

## Four Condition Types × Four Source Types = 16 Entries

**Exception Condition Types:**
1. Synchronous — deliberate (SVC syscall, data abort, instruction abort, undefined instruction)
2. IRQ — external interrupt from GIC (Generic Interrupt Controller)
3. FIQ — fast interrupt (rarely used in Linux; routed to EL3/secure)
4. SError — asynchronous system error (hardware fault)

**Source Types:**
1. Current EL, SP_EL0 — kernel code using SP_EL0 (Linux doesn't use this mode)
2. Current EL, SP_ELx — kernel code using SP_EL1 ← **Linux uses this**
3. Lower EL, AArch64 — 64-bit user code ← **Linux uses this for syscalls/user faults**
4. Lower EL, AArch32 — 32-bit user code (compat mode)

Linux primarily handles exceptions from two banks:
- **Bank 2 (offset 0x200–0x380):** Kernel exceptions (page faults, undefined instructions)
- **Bank 3 (offset 0x400–0x580):** User exceptions (syscalls, user page faults, user IRQs)

---

## Exception Dispatch Flow for a Syscall

```
User process executes SVC #0  (syscall instruction)
    │
    ▼
CPU: save context, jump to VBAR_EL1 + 0x400 (lower EL, AArch64, sync)
    │
    ▼
kernel_ventry macro code (128 bytes):
    sub sp, sp, #PT_REGS_SIZE       // reserve pt_regs on kernel stack
    b   el0t_64_sync                // branch to real handler
    │
    ▼
el0t_64_sync (arch/arm64/kernel/entry.S):
    kernel_entry 0, 64              // save all registers to pt_regs
    mrs x22, elr_el1                // x22 = PC of user instruction after SVC
    mrs x25, spsr_el1               // x25 = user PSTATE
    bl  el0_sync_handler            // C handler
    │
    ▼
el0_sync_handler (arch/arm64/kernel/entry-common.c):
    u64 esr = read_sysreg(esr_el1);
    if (esr_ec == ESR_ELx_EC_SVC64)
        el0_svc(regs);              // → syscall dispatch
```

---

## `vectors` Alignment — Why 11-Bit?

`VBAR_EL1` must be aligned to `2^11 = 2048` bytes. This is an ARM architecture
requirement (VBAR_EL1 bits [10:0] are RES0 = read as zero, write ignored).

The linker ensures this:
```asm
// entry.S:
    .align  11    // align to 2^11 = 2048 bytes
SYM_CODE_START(vectors)
```

If `vectors` were not 2048-byte aligned:
- `msr vbar_el1, x8` would be UNPREDICTABLE (architecture violation)
- In practice, the CPU silently ignores bits [10:0] and uses a misaligned address
- First exception → CPU jumps to wrong address → immediate crash

---

## VBAR_EL1 vs VBAR_EL2

If the kernel runs under KVM or with VHE:
- `vbar_el2` is used for EL2 exceptions
- Linux sets `vbar_el2` to the KVM vectors (hypervisor handlers)
- `vbar_el1` still set to `vectors` (for EL1 exceptions)

`__primary_switched` only sets `vbar_el1`. The `finalise_el2` call (later in
`__primary_switched`) may set `vbar_el2` if needed for VHE.

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