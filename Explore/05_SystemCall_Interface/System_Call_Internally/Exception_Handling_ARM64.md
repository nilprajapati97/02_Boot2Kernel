# ARM64 (AArch64) Exception Handling in the Linux Kernel — Deep Dive

An **exception** on ARM64 is any event that forces the CPU to stop executing the
current instruction stream and jump to a fixed, privileged handler. It is the
*single, unified mechanism* the ARM architecture uses for **everything** that
breaks normal flow: a system call (`SVC`), a page fault, a divide trap, a timer
interrupt, a device IRQ, an asynchronous memory error — they are all
"exceptions". Linux builds its entire kernel-entry machinery on top of this one
hardware concept.

This document explains, from scratch and in depth, **how the ARM64 exception
model works** and **how Linux uses it** — framed around **Why? When? Where?
How?** It is the architectural foundation underneath the
[System Call Interface](../SystemCall_Interface.md): a syscall is just *one kind*
of synchronous exception.

> Scope: AArch64 (64-bit ARM, `arm64` in the kernel tree). Kernel source paths
> refer to a modern mainline tree (~v6.x). Where the layout changed historically
> (the old monolithic `entry.S` vs. today's split `entry.S` + `entry-common.c`),
> both are noted.

---

## 0. Table of Contents

1. [The Big Picture (Layers)](#1-the-big-picture-layers)
2. [WHY Exceptions Exist (Privilege Separation)](#2-why-exceptions-exist-privilege-separation)
3. [WHEN Exceptions Fire (Taxonomy)](#3-when-exceptions-fire-taxonomy)
4. [WHERE: Exception Levels EL0–EL3 & Register Banking](#4-where-exception-levels-el0el3--register-banking)
5. [The Vector Table (VBAR_EL1) — 16 Entries](#5-the-vector-table-vbar_el1--16-entries)
6. [The Syndrome & State Registers (ESR/ELR/SPSR/FAR/DAIF)](#6-the-syndrome--state-registers-esrelrspsrfardaif)
7. [HOW (Entry): What the Hardware Does + `kernel_entry`](#7-how-entry-what-the-hardware-does--kernel_entry)
8. [Synchronous Path 1 — SVC (System Call)](#8-synchronous-path-1--svc-system-call)
9. [Synchronous Path 2 — Aborts (Page Faults)](#9-synchronous-path-2--aborts-page-faults)
10. [Asynchronous Path — IRQ & the GIC](#10-asynchronous-path--irq--the-gic)
11. [FIQ & SError](#11-fiq--serror)
12. [HOW (Exit): `kernel_exit`, `ret_to_user`, and `ERET`](#12-how-exit-kernel_exit-ret_to_user-and-eret)
13. [`pt_regs`, Context & Preemption](#13-pt_regs-context--preemption)
14. [Nesting, Masking (DAIF) & Safety](#14-nesting-masking-daif--safety)
15. [Quick-Reference Cheat Sheets](#15-quick-reference-cheat-sheets)

---

## 1. The Big Picture (Layers)

```
┌──────────────────────────────────────────────────────────────┐
│ User Application (your C program)                            │  EL0
│   read(fd, buf, n);   *p = 5;   (timer ticks in background)  │
├──────────────────────────────────────────────────────────────┤
│ libc wrapper → executes  SVC #0   (for syscalls)             │  EL0
├──────────────────────────────────────────────────────────────┤
│ ░░░░░  EXCEPTION TAKEN (hardware)  ░░░░░                     │  ← privilege jump EL0→EL1
│   • CPU computes vector address = VBAR_EL1 + offset          │
│   • Saves PC→ELR_EL1, PSTATE→SPSR_EL1, cause→ESR_EL1         │
│   • Masks D,A,I,F; selects SP_EL1; PC ← vector entry         │
├──────────────────────────────────────────────────────────────┤
│ Vector table entry (arch/arm64/kernel/entry.S: `vectors`)    │  EL1
│   one of 16 fixed 128-byte slots                             │
├──────────────────────────────────────────────────────────────┤
│ `kernel_entry` macro: build pt_regs frame on kernel stack    │  EL1
├──────────────────────────────────────────────────────────────┤
│ C dispatch (arch/arm64/kernel/entry-common.c)                │  EL1
│   el0t_64_sync_handler / el0t_64_irq_handler / ...           │
├──────────────────────────────────────────────────────────────┤
│ Specific handler:                                            │  EL1
│   • SVC      → el0_svc → invoke_syscall → sys_xxx()          │
│   • abort    → do_mem_abort → do_page_fault                  │
│   • IRQ      → gic_handle_irq → generic_handle_domain_irq    │
├──────────────────────────────────────────────────────────────┤
│ `ret_to_user` → `kernel_exit` → restore regs → `ERET`        │  EL1→EL0
└──────────────────────────────────────────────────────────────┘
                        back to EL0, exactly where it left off
```

Key mental model: **the kernel never "calls" itself to enter — the CPU
*throws* it there.** Linux's job is to (1) place a vector table at a known
address, (2) save enough state to resume later, (3) figure out *why* it was
entered, (4) handle it, (5) restore state and `ERET` back.

---

## 2. WHY Exceptions Exist (Privilege Separation)

A CPU that runs untrusted user code next to the OS needs a wall between them and
a *single, guarded gate* through that wall. Exceptions are that gate.

Two properties make them safe:

1. **Privilege escalation is atomic and controlled.** Taking an exception raises
   the Exception Level (e.g., EL0 → EL1) *at the same instant* it transfers
   control. User code can never "be at EL1 but running its own instructions".
2. **The entry point is fixed by the kernel, not the caller.** The CPU jumps to
   `VBAR_EL1 + offset`. User code cannot choose *where* in the kernel to land —
   it can only choose *which category* of exception to raise (and even that only
   for `SVC`).

Without this, any program could read another process's memory, reprogram the
MMU, mask interrupts, or halt the machine. The ARM exception model — combined
with the MMU and Exception Levels — is what makes a multi-user, multi-process OS
possible.

> **Contrast with x86:** x86 uses *rings* (0–3) and an *IDT* (Interrupt
> Descriptor Table) with the `syscall`/`int 0x80` instructions. ARM64 unifies
> traps, interrupts, and syscalls under one "exception" abstraction with
> *Exception Levels* and a *vector table*. The concept is the same; the
> spelling differs.

---

## 3. WHEN Exceptions Fire (Taxonomy)

ARM64 has exactly **four exception classes**. Everything that enters the kernel
is one of these:

| Class | Trigger | Sync? | Typical cause | Linux entry |
|-------|---------|-------|---------------|-------------|
| **Synchronous** | The *current instruction* directly caused it | Yes | `SVC` (syscall), data/instruction abort (page fault), undefined instruction, alignment fault, `BRK` (debug), FP trap | `el0_sync` / `el1_sync` |
| **IRQ** (Interrupt Request) | External device, asynchronous | No | Timer tick, disk/NIC/keyboard IRQ delivered via the GIC | `el0_irq` / `el1_irq` |
| **FIQ** (Fast Interrupt) | External, higher priority than IRQ | No | Secure-world / specialized interrupts (rarely used by normal Linux) | `el0_fiq` / `el1_fiq` |
| **SError** (System Error) | Asynchronous hardware fault | No | Uncorrectable ECC memory error, async external abort | `el0_error` / `el1_error` |

**Synchronous vs. Asynchronous** is the single most important distinction:

- **Synchronous**: precisely tied to *one instruction*. The faulting PC is in
  `ELR_EL1`, and `ESR_EL1` tells you *exactly* what happened. Re-executable
  (e.g., after fixing a page fault, you re-run the load).
- **Asynchronous** (IRQ/FIQ/SError): arrives "between" instructions; not caused
  by the program. The interrupted PC is whatever was running.

A few concrete "when" examples:

| Program did... | Exception |
|----------------|-----------|
| `svc #0` (libc `write`) | Synchronous — SVC |
| `ldr x0, [x1]` where `x1` page not present | Synchronous — Data Abort (page fault) |
| Jump to an unmapped/NX page | Synchronous — Instruction Abort |
| Executed an undefined opcode | Synchronous — Undefined Instruction |
| Nothing — timer expired | IRQ |
| Nothing — RAM returned uncorrectable ECC error | SError |

---

## 4. WHERE: Exception Levels EL0–EL3 & Register Banking

### 4.1 The four Exception Levels

ARM64 defines four privilege tiers, called **Exception Levels (ELx)**. Higher
number = more privilege:

```
            ┌───────────────────────────────────────────────┐
  EL3       │ Secure Monitor (ARM Trusted Firmware, BL31)   │  highest
            │ owns the Secure/Non-secure world switch       │
            ├───────────────────────────────────────────────┤
  EL2       │ Hypervisor (KVM, Xen)                         │
            │ virtualization, nested page tables (stage 2)  │
            ├───────────────────────────────────────────────┤
  EL1       │ OS Kernel  ◄── the Linux kernel lives here    │
            │ MMU, scheduler, drivers, syscall handlers     │
            ├───────────────────────────────────────────────┤
  EL0       │ User applications (unprivileged)              │  lowest
            └───────────────────────────────────────────────┘
```

| EL | Who | Examples |
|----|-----|----------|
| EL0 | User space | your app, libc, shells |
| EL1 | OS kernel | Linux, drivers |
| EL2 | Hypervisor | KVM host, `kvm-arm` |
| EL3 | Secure firmware | ARM Trusted Firmware, PSCI, secure boot |

Linux normally runs the **kernel at EL1** and **user space at EL0**. When KVM is
enabled, parts of the host also use EL2 (VHE — Virtualization Host Extensions —
lets the kernel run at EL2 directly). Exceptions can only be taken to the *same
or higher* EL — never to a lower one. To go *down* (e.g., EL1 → EL0), you use
`ERET`.

### 4.2 Banked registers — the magic that makes return possible

Each Exception Level (1–3) has its own **banked** copies of three critical
registers. "Banked" means the hardware keeps separate physical copies, selected
automatically by the current EL, so taking an exception does not destroy the
state of the level you came from.

| Register | Meaning | Set by HW on exception entry |
|----------|---------|------------------------------|
| `ELR_ELx` | **Exception Link Register** — the PC to return to | ← preferred return address |
| `SPSR_ELx` | **Saved Program Status Register** — snapshot of `PSTATE` | ← caller's PSTATE (flags, EL, SP sel, DAIF) |
| `SP_ELx` | **Stack Pointer** for level x | (selected; not overwritten) |
| `ESR_ELx` | **Exception Syndrome Register** — *why* | ← cause/syndrome |
| `FAR_ELx` | **Fault Address Register** — faulting VA | ← address (for aborts) |
| `VBAR_ELx` | **Vector Base Address Register** | (read to find the vector) |

So when an exception is taken to EL1, the hardware fills in `ELR_EL1`,
`SPSR_EL1`, `ESR_EL1` (and `FAR_EL1` for aborts). To resume, Linux eventually
executes `ERET`, which restores `PC ← ELR_EL1` and `PSTATE ← SPSR_EL1` in one
atomic step.

### 4.3 Stack pointer selection: SP_EL0 vs SP_ELx

AArch64 has a clever stack-pointer scheme. At each EL you can choose to use
either:

- **`SP_EL0`** — a single shared SP (the "thread" SP), or
- **`SP_ELx`** — the EL's *own* dedicated SP (the "handler" SP).

This choice is the `SPSel` bit in `PSTATE`, and it gives rise to the **`t` vs
`h`** naming you see in the vector table:

- **`EL1t`** — EL1 using `SP_EL0` (the **t**hread stack).
- **`EL1h`** — EL1 using `SP_EL1` (the **h**andler stack).

Linux runs the kernel as **EL1h** (kernel uses its own `SP_EL1`), while user
space runs as **EL0t**. This separation means a fault taken from EL0 lands the
CPU on a *trusted kernel stack*, never on the (possibly malicious or corrupt)
user stack.

---

## 5. The Vector Table (VBAR_EL1) — 16 Entries

When an exception is taken to EL1, the CPU computes the target address as:

```
target = VBAR_EL1 + offset
```

`VBAR_EL1` (Vector Base Address Register) holds the base of the table; Linux
sets it during early boot to point at the `vectors` symbol. The table is **2 KB
aligned** and contains **16 entries**, each **128 bytes (0x80)** apart — large
enough to hold a short stub that branches to the real handler.

### 5.1 The 4 × 4 layout

The 16 entries are organized as **4 groups of 4**. The *group* encodes *where
the exception came from*; the *entry within the group* encodes *which class*:

```
            Offset    Group (source of exception)
            ──────    ─────────────────────────────────────────────
            0x000  ┐
            0x080  │   Group 1: Current EL, using SP_EL0   (EL1t)
            0x100  │            (kernel was on the thread SP — rare)
            0x180  ┘
            ──────
            0x200  ┐
            0x280  │   Group 2: Current EL, using SP_ELx   (EL1h)
            0x300  │            ◄── kernel-mode faults land here
            0x380  ┘
            ──────
            0x400  ┐
            0x480  │   Group 3: Lower EL, AArch64          (EL0 64-bit)
            0x500  │            ◄── user-mode syscalls/faults land here
            0x580  ┘
            ──────
            0x600  ┐
            0x680  │   Group 4: Lower EL, AArch32          (EL0 32-bit compat)
            0x700  │
            0x780  ┘

  Within each group, the 4 entries are (in order):
     +0x000  Synchronous
     +0x080  IRQ
     +0x100  FIQ
     +0x180  SError
```

So, for example, a **system call from a 64-bit user app** is a *synchronous*
exception from a *lower EL in AArch64*, landing at **`VBAR_EL1 + 0x400`**. A
**page fault while the kernel itself runs** lands at **`VBAR_EL1 + 0x200`**.

### 5.2 The kernel's vector table in source

In `arch/arm64/kernel/entry.S`:

```asm
        .pushsection ".entry.text", "ax"
        .align  11                       // 2 KB aligned (VBAR requirement)
SYM_CODE_START(vectors)
        kernel_ventry   1, t, 64, sync   // 0x000  EL1t synchronous
        kernel_ventry   1, t, 64, irq    // 0x080  EL1t IRQ
        kernel_ventry   1, t, 64, fiq    // 0x100  EL1t FIQ
        kernel_ventry   1, t, 64, error  // 0x180  EL1t SError

        kernel_ventry   1, h, 64, sync   // 0x200  EL1h synchronous (kernel)
        kernel_ventry   1, h, 64, irq    // 0x280  EL1h IRQ
        kernel_ventry   1, h, 64, fiq    // 0x300  EL1h FIQ
        kernel_ventry   1, h, 64, error  // 0x380  EL1h SError

        kernel_ventry   0, t, 64, sync   // 0x400  EL0 AArch64 sync (syscall!)
        kernel_ventry   0, t, 64, irq    // 0x480  EL0 AArch64 IRQ
        kernel_ventry   0, t, 64, fiq    // 0x500  EL0 AArch64 FIQ
        kernel_ventry   0, t, 64, error  // 0x580  EL0 AArch64 SError

        kernel_ventry   0, t, 32, sync   // 0x600  EL0 AArch32 sync
        kernel_ventry   0, t, 32, irq    // 0x680  EL0 AArch32 IRQ
        kernel_ventry   0, t, 32, fiq    // 0x700  EL0 AArch32 FIQ
        kernel_ventry   0, t, 32, error  // 0x780  EL0 AArch32 SError
SYM_CODE_END(vectors)
```

The `kernel_ventry` macro expands each slot into a small stub that (after
hardening fix-ups) branches to a C handler such as `el0t_64_sync_handler` or
`el1h_64_irq_handler`. Modern kernels generate these names via the
`entry_handler` macro and define the C bodies in
`arch/arm64/kernel/entry-common.c`.

> **Older trees**: pre-v5.8 kernels branched directly to assembly labels named
> `el0_sync`, `el1_irq`, etc. The split into thin asm vectors + C handlers
> (`entry-common.c`) was done to move logic into reviewable C and to centralize
> entry/exit bookkeeping (`enter_from_user_mode()` / `exit_to_user_mode()`).

---

## 6. The Syndrome & State Registers (ESR/ELR/SPSR/FAR/DAIF)

These registers are how the kernel *decodes* an exception once it lands.

### 6.1 `ESR_EL1` — Exception Syndrome Register

The most important diagnostic register. It tells the kernel **what kind** of
synchronous exception happened, in a packed bit field:

```
 63          32 31    26 25 24                       0
┌──────────────┬────────┬──┬─────────────────────────┐
│   (RES0/...) │   EC   │IL│          ISS            │
└──────────────┴────────┴──┴─────────────────────────┘
                 6 bits   1        25 bits
```

- **`EC` (Exception Class, bits 26–31)** — the primary "what happened" code.
- **`IL` (Instruction Length, bit 25)** — 0 = 16-bit (Thumb), 1 = 32-bit.
- **`ISS` (Instruction Specific Syndrome, bits 0–24)** — extra detail whose
  meaning depends on `EC` (e.g., for a data abort: was it a read or write? what
  fault status code?).

Common `EC` values (`arch/arm64/include/asm/esr.h`):

| EC value | Symbol | Meaning |
|----------|--------|---------|
| `0x15` | `ESR_ELx_EC_SVC64` | `SVC` from AArch64 — **a system call** |
| `0x24` | `ESR_ELx_EC_DABT_LOW` | Data abort from a lower EL (user page fault) |
| `0x25` | `ESR_ELx_EC_DABT_CUR` | Data abort from current EL (kernel page fault) |
| `0x20` | `ESR_ELx_EC_IABT_LOW` | Instruction abort from lower EL |
| `0x21` | `ESR_ELx_EC_IABT_CUR` | Instruction abort from current EL |
| `0x00` | `ESR_ELx_EC_UNKNOWN` | Undefined / unknown |
| `0x07` | `ESR_ELx_EC_SVE`/SIMD-FP | FP/SIMD/SVE access trap |
| `0x3C` | `ESR_ELx_EC_BRK64` | `BRK` software breakpoint (debug) |

The kernel reads `ESR_EL1`, extracts `EC` (e.g., `ESR_ELx_EC(esr)`), and uses it
to pick the right handler. For the syscall path this looks like:

```c
// arch/arm64/kernel/entry-common.c (simplified)
switch (ESR_ELx_EC(esr)) {
case ESR_ELx_EC_SVC64:
        el0_svc(regs);                  // → system call
        break;
case ESR_ELx_EC_DABT_LOW:
        el0_da(regs, esr);              // → do_mem_abort (data abort)
        break;
case ESR_ELx_EC_IABT_LOW:
        el0_ia(regs, esr);              // → instruction abort
        break;
/* ... FP, SVE, undef, BRK, etc. ... */
}
```

### 6.2 `ELR_EL1` — Exception Link Register

Holds the **preferred return address**: for a synchronous exception, the address
of the *faulting* instruction (so it can be retried) or the *next* instruction
(for `SVC`, it points *after* the `svc`, so the syscall doesn't loop). `ERET`
restores `PC` from here.

### 6.3 `SPSR_EL1` — Saved Program Status Register

A frozen copy of `PSTATE` from the instant the exception was taken. Key fields:

```
 31 30 29 28 27  ...  9  8  7  6  5  4  3 2 1 0
┌──┬──┬──┬──┬──┐    ┌──┬──┬──┬──┬──┐ ┌─────────┐
│ N│ Z│ C│ V│..│    │ D│ A│ I│ F│..│ │  M[4:0] │
└──┴──┴──┴──┴──┘    └──┴──┴──┴──┴──┘ └─────────┘
  condition flags     DAIF masks       mode (EL & SP sel)
```

- **N/Z/C/V** — condition flags.
- **D, A, I, F** — the **DAIF** interrupt-mask bits at the time of entry
  (Debug, SError, IRQ, FIQ).
- **M[4:0]** — the EL and stack selection to return to (e.g., `0b00000` = EL0t,
  `0b00101` = EL1h). `ERET` uses this to drop back to the correct EL.

### 6.4 `FAR_EL1` — Fault Address Register

For data/instruction aborts and alignment faults, this holds the **faulting
virtual address**. The page-fault handler reads it to know *which address* the
program touched. (`ESR` says "it was a write data abort"; `FAR` says "to address
`0xffff_0000_1234`".)

### 6.5 `DAIF` — the interrupt mask bits

`DAIF` is part of `PSTATE` (not a syndrome register, but central to exception
behavior):

| Bit | Masks | Notes |
|-----|-------|-------|
| **D** | Debug exceptions | watchpoints/breakpoints |
| **A** | SError (async abort) | |
| **I** | IRQ | what `local_irq_disable()` toggles |
| **F** | FIQ | |

On exception entry the hardware **sets D, A, I, F** (masks everything) so the
handler starts in a quiet, non-reentrant state. Linux then selectively unmasks
(e.g., re-enables IRQs once it's safe). Setting a DAIF bit = "mask/ignore";
clearing = "allow".

---

## 7. HOW (Entry): What the Hardware Does + `kernel_entry`

### 7.1 What the CPU does automatically (in hardware)

When an exception is taken to EL1, the CPU performs these steps **atomically**,
before a single kernel instruction runs:

1. `SPSR_EL1 ← PSTATE`  (snapshot the caller's processor state)
2. `ELR_EL1  ← preferred return address`  (faulting or next PC)
3. `ESR_EL1  ← syndrome`  (cause); `FAR_EL1 ← address` for aborts
4. `PSTATE.{D,A,I,F} ← 1`  (mask all interrupts)
5. `PSTATE.EL ← 1`, `PSTATE.SP ← 1`  (raise to EL1, select `SP_EL1`)
6. `PC ← VBAR_EL1 + offset`  (jump to the matching vector slot)

Note what the hardware does **not** do: it does not save the general-purpose
registers `x0`–`x30`, and it does not switch page tables. Those are software's
responsibility.

### 7.2 `kernel_entry` — building the `pt_regs` frame

The vector stub branches into the `kernel_entry` macro (in `entry.S`), which
saves the full register file onto the kernel stack, forming a **`struct
pt_regs`** — the canonical "snapshot of user context" that the rest of the
kernel works with:

```asm
        .macro  kernel_entry, el, regsize = 64
        sub     sp, sp, #PT_REGS_SIZE       // make room for pt_regs
        stp     x0,  x1,  [sp, #16 * 0]     // save x0..x29 in pairs
        stp     x2,  x3,  [sp, #16 * 1]
        ...
        stp     x28, x29, [sp, #16 * 14]

        .if     \el == 0
        mrs     x21, sp_el0                 // user SP came from SP_EL0
        ...
        .else
        add     x21, sp, #PT_REGS_SIZE      // kernel SP
        .endif

        mrs     x22, elr_el1                // saved PC
        mrs     x23, spsr_el1               // saved PSTATE
        stp     lr,  x21, [sp, #S_LR]       // x30 + sp
        stp     x22, x23, [sp, #S_PC]       // pc + pstate
        .endm
```

After `kernel_entry`, the kernel stack holds a complete `pt_regs`, and `sp`
points at it. That pointer is passed (in `x0`) to the C handler. Crucially, for
an EL0 exception the code reads the **user** SP from `SP_EL0` and switches to the
**kernel** `SP_EL1` — so the handler runs on a trusted, per-task kernel stack.

### 7.3 Hardening shims around entry

Real entry code also performs security/robustness fix-ups before the C handler:

- **KPTI (Kernel Page Table Isolation)** — like x86 Meltdown mitigation, user
  space runs with a trimmed page table; on entry the kernel swaps `TTBR1`/`TTBR0`
  to map the full kernel. Handled via the `tramp_*` trampoline vectors.
- **Shadow Call Stack / PAC / BTI** — pointer-auth and control-flow integrity
  fix-ups.
- **`enter_from_user_mode()`** — context-tracking / RCU / lockdep bookkeeping so
  the kernel knows it transitioned from user to kernel.

---

## 8. Synchronous Path 1 — SVC (System Call)

This is the case the [System Call Interface](../SystemCall_Interface.md) doc
covers end-to-end; here is the *exception-side* view.

### 8.1 The trigger

A 64-bit user program executes:

```asm
        mov  x8, #64          // syscall number (write = 64 on arm64)
        mov  x0, #1           // arg0: fd
        ldr  x1, =msg         // arg1: buf
        mov  x2, #3           // arg2: count
        svc  #0               // ← synchronous exception, EC = SVC64
```

ARM64 syscall ABI: **`x8` = syscall number**, **`x0`–`x5` = args**, **`x0` =
return value** on the way back.

### 8.2 The path

```
svc #0 (EL0)
   │  hardware: ESR_EL1.EC = 0x15 (SVC64), enter at VBAR_EL1 + 0x400
   ▼
vectors[0x400]  →  kernel_entry (build pt_regs)
   ▼
el0t_64_sync_handler(regs)            // entry-common.c
   ▼  switch (EC) → case SVC64:
el0_svc(regs)                         // syscall.c
   ▼
do_el0_svc(regs) → el0_svc_common()   // reads syscall nr from regs->regs[8]
   ▼
invoke_syscall(regs, nr, sc_nr, syscall_table)
   ▼
sys_call_table[nr](regs)              // e.g., __arm64_sys_write
   ▼
ksys_write() → vfs_write() → ...      // generic kernel work
```

`invoke_syscall` (in `arch/arm64/kernel/syscall.c`) bounds-checks the number
against `__NR_syscalls`, then indexes the architecture's `sys_call_table` (built
from `arch/arm64/include/asm/unistd.h` / the generic syscall table). The result
is written back into `regs->regs[0]` (which becomes `x0` for the user on return).

> Syscall numbers for arm64 come from the **generic** syscall table
> (`include/uapi/asm-generic/unistd.h`), unlike x86 which keeps its own `.tbl`.
> This is why `__NR_write` etc. are shared across newer architectures.

---

## 9. Synchronous Path 2 — Aborts (Page Faults)

A **page fault** is just a synchronous *abort* exception — the MMU could not
translate (or permission-check) a virtual address.

### 9.1 The trigger

```asm
        ldr  x0, [x1]      // x1 holds a VA whose page is not mapped/present
                          // → Data Abort, EC = 0x24 (DABT from lower EL)
```

The hardware records the *type* in `ESR_EL1` and the *address* in `FAR_EL1`.

### 9.2 The path

```
ldr fault (EL0)
   │  hardware: ESR.EC = 0x24 (DABT_LOW), FAR_EL1 = faulting VA,
   │            enter at VBAR_EL1 + 0x400
   ▼
el0t_64_sync_handler → case DABT_LOW → el0_da(regs, esr)
   ▼
do_mem_abort(far, esr, regs)          // arch/arm64/mm/fault.c
   ▼  look up fault_info[] by ESR fault status code (DFSC)
do_page_fault(far, esr, regs)
   ▼
   find_vma()  → is the address in a valid VMA?
   ├─ yes, just not present  → handle_mm_fault() → allocate/map page → retry
   ├─ COW / write to RO page → copy-on-write, install writable page
   └─ no valid VMA / bad perm → SIGSEGV to the process
```

Decoding within `ESR.ISS` for a data abort:

| ISS field | Meaning |
|-----------|---------|
| **WnR** (bit 6) | Write-not-Read: 1 = the access was a store |
| **DFSC** (bits 0–5) | Data Fault Status Code: translation fault, permission fault, alignment, etc. |
| **FnV** | FAR not Valid (rare) |

The beauty of *synchronous* aborts: because `ELR_EL1` points at the faulting
instruction, once the kernel makes the page valid it simply `ERET`s and the CPU
**re-executes the same load/store**, which now succeeds. The user program never
knows a fault happened (this is how demand paging, mmap, and copy-on-write are
implemented).

If the kernel itself faults (`EC = 0x25`, DABT_CUR, from EL1), the path goes
through `el1h_64_sync_handler` → `do_mem_abort`; an unrecoverable kernel fault
produces the famous **"Unable to handle kernel paging request"** oops.

---

## 10. Asynchronous Path — IRQ & the GIC

Interrupts are *asynchronous* exceptions delivered by the **GIC (Generic
Interrupt Controller)** — the standard ARM interrupt-routing hardware.

### 10.1 The GIC architecture

```
   ┌───────────┐  ┌───────────┐  ┌───────────┐   devices raise SPIs
   │  Timer    │  │   NIC     │  │   UART    │   (Shared Peripheral Ints)
   └─────┬─────┘  └─────┬─────┘  └─────┬─────┘
         └──────────────┼──────────────┘
                        ▼
            ┌───────────────────────────┐
            │   GIC Distributor (GICD)  │  prioritizes & routes interrupts
            └─────────────┬─────────────┘
                          │  (GICv3: per-CPU Redistributor GICR)
        ┌─────────────────┼──────────────────┐
        ▼                 ▼                  ▼
   ┌─────────┐       ┌─────────┐       ┌─────────┐
   │ CPU0 IF │       │ CPU1 IF │  ...  │ CPUn IF │   CPU interface
   └────┬────┘       └─────────┘       └─────────┘   (GICv3: system regs ICC_*)
        │  asserts the IRQ signal to the core
        ▼
   Core takes an IRQ exception → VBAR_EL1 + (0x480 if from EL0)
```

Interrupt ID ranges:

| Type | IDs | Meaning |
|------|-----|---------|
| **SGI** | 0–15 | Software Generated Interrupt (inter-processor, e.g., IPI/reschedule) |
| **PPI** | 16–31 | Private Peripheral Interrupt (per-CPU, e.g., the arch timer) |
| **SPI** | 32–1019 | Shared Peripheral Interrupt (devices: NIC, disk, UART) |
| **LPI** | 8192+ | Locality-specific (message-signaled, via the ITS) — GICv3 |

**GICv2 vs GICv3:** GICv2 uses memory-mapped CPU-interface registers (`GICC_*`)
and supports up to 8 CPUs. GICv3 replaces the CPU interface with **system
registers** (`ICC_IAR1_EL1`, `ICC_EOIR1_EL1`, …), adds per-CPU
**Redistributors (GICR)** and the **ITS** for MSI/LPIs, and scales to thousands
of cores. Most modern arm64 systems are GICv3/v4.

### 10.2 The IRQ path in Linux

```
device asserts IRQ  →  GIC routes  →  core takes IRQ exception
   │  hardware: enter at VBAR_EL1 + 0x480 (from EL0) or 0x280 (from EL1)
   ▼
vectors[irq]  →  kernel_entry  →  el0t_64_irq_handler / el1h_64_irq_handler
   ▼
do_interrupt_handler(regs, handle_arch_irq)
   ▼
gic_handle_irq(regs)                  // drivers/irqchip/irq-gic-v3.c
   │   read ICC_IAR1_EL1 → get the interrupt ID (INTID); acknowledge
   ▼
generic_handle_domain_irq(domain, hwirq)   // kernel/irq/irqdesc.c
   ▼
irq_desc → handle_irq_event → action->handler(irq, dev_id)   // your driver's ISR
   ▼
   (driver does minimal work, often schedules softirq/threaded IRQ/tasklet)
   ▼
write ICC_EOIR1_EL1 (End Of Interrupt) → ret_to_user/kernel → ERET
```

Key points:

- The kernel runs on a dedicated **IRQ stack** (or the task's kernel stack) with
  IRQs initially masked; it acknowledges the interrupt to the GIC (read the IAR),
  runs the registered handler, then signals **EOI**.
- Heavy work is deferred to **softirqs**, **tasklets**, or **threaded IRQs** to
  keep the hard-IRQ path short. This is the classic "top half / bottom half"
  split.
- The same exception machinery (pt_regs, kernel_entry/exit) is reused — only the
  vector slot and the dispatch function differ.

---

## 11. FIQ & SError

### 11.1 FIQ (Fast Interrupt)

Architecturally identical to IRQ but a *separate, higher-priority* signal with
its own DAIF mask (`F`). Historically meant for low-latency handlers. In
mainline Linux on normal (non-secure) systems, FIQs are usually **routed to the
secure world (EL3 firmware)** or unused, so the FIQ vectors typically lead to a
"bad mode" panic if they ever fire unexpectedly. Some platforms (e.g., certain
pseudo-NMI setups using GICv3 interrupt priorities) leverage FIQ-like behavior
for **NMI** support.

### 11.2 SError (System Error)

An **asynchronous abort** signaling a serious hardware condition — typically an
**uncorrectable memory error (ECC)** or an external abort that couldn't be tied
to a specific instruction. Masked by the `A` bit in DAIF.

```
SError → VBAR_EL1 + (0x580 from EL0 / 0x380 from EL1)
   ▼
el0t_64_error_handler / el1h_64_error_handler
   ▼
do_serror(regs, esr)                  // arch/arm64/kernel/traps.c
   ▼
   RAS-aware: try to identify/contain (APEI/GHES); often fatal → panic or SIGBUS
```

Because SErrors are asynchronous, they can't simply be "retried"; the kernel
uses **RAS (Reliability, Availability, Serviceability)** extensions to triage —
sometimes it can attribute the error to a process and deliver `SIGBUS`, otherwise
it panics.

---

## 12. HOW (Exit): `kernel_exit`, `ret_to_user`, and `ERET`

Returning from an exception reverses the entry, but with an important
*work-check* loop on the way back to user space.

### 12.1 `ret_to_user` — the work loop

Before going back to EL0, the kernel checks the thread's `TIF_*` flags
(`_TIF_WORK_MASK`) to see if any pending work must run first:

```
do_notify_resume(regs, thread_flags):
   • _TIF_NEED_RESCHED      → schedule()        (preempt: run another task)
   • _TIF_SIGPENDING        → do_signal()       (deliver pending signals)
   • _TIF_NOTIFY_RESUME     → resume_user_mode_work()  (task_work, etc.)
   • _TIF_FOREIGN_FPSTATE   → restore FP/SIMD state
```

This is *the* place where preemption and signal delivery happen — every return
to user space is a scheduling opportunity. The loop repeats until no work
remains, then proceeds to the real exit.

### 12.2 `kernel_exit` — restoring `pt_regs`

The mirror image of `kernel_entry`: it reloads `ELR_EL1` and `SPSR_EL1` from the
saved `pt_regs`, restores `x0`–`x30`, restores the user SP into `SP_EL0`, undoes
the KPTI/page-table swap, and finally executes:

```asm
        ldp     x21, x22, [sp, #S_PC]   // load saved pc, pstate
        msr     elr_el1, x21            // ELR_EL1 ← return PC
        msr     spsr_el1, x22           // SPSR_EL1 ← return PSTATE
        ldp     x0,  x1,  [sp, #16 * 0] // restore GPRs...
        ...
        eret                            // ← atomic: PC←ELR, PSTATE←SPSR, drop EL
```

### 12.3 `ERET`

`ERET` is the single instruction that *atomically*:

1. Restores `PC ← ELR_EL1`.
2. Restores `PSTATE ← SPSR_EL1` (which lowers the EL — e.g., EL1 → EL0 — and
   restores DAIF, flags, and SP selection).

After `ERET`, the CPU is back in user mode at exactly the instruction following
the one that trapped (for `SVC`) or re-executing the faulting one (for an abort).
Modern cores also require `eret` to be combined with speculation barriers
(`dsb`/`isb`, or the `ERETAA`/`ERETAB` pointer-authenticated variants) for
Spectre-class hardening.

---

## 13. `pt_regs`, Context & Preemption

### 13.1 `struct pt_regs`

Defined in `arch/arm64/include/asm/ptrace.h`, this is the in-memory layout that
`kernel_entry` builds and `kernel_exit` consumes:

```c
struct pt_regs {
        union {
                struct user_pt_regs user_regs;
                struct {
                        u64 regs[31];   // x0 .. x30
                        u64 sp;         // user SP (from SP_EL0)
                        u64 pc;         // ELR_EL1  (return address)
                        u64 pstate;     // SPSR_EL1 (saved PSTATE)
                };
        };
        u64 orig_x0;                    // original x0 (for syscall restart)
        s32 syscallno;                  // syscall number, or -1
        u32 unused2;
        u64 sdei_ttbr1;
        u64 pmr_save;                   // GICv3 priority mask (pseudo-NMI)
        u64 stackframe[2];
        ...
};
```

This single structure *is* the saved user context. `ptrace`, core dumps, signal
frames, and `/proc/<pid>/syscall` all read it. Passing a `pt_regs *` to handlers
is why modern syscall handlers have the `__arm64_sys_xxx(struct pt_regs *)`
signature.

### 13.2 Kernel stacks

Each task has its own **kernel stack** (`THREAD_SIZE`, typically 16 KB). On EL0
entry the CPU switches from `SP_EL0` (user stack) to `SP_EL1` (kernel stack);
the `pt_regs` frame is built at the *top* of that kernel stack. There are also
dedicated per-CPU stacks for special contexts: the **IRQ stack**, the
**overflow stack** (for stack-overflow detection), and the **SDEI** stack.

### 13.3 Preemption

If `CONFIG_PREEMPT` is set, even a return *from a kernel-mode interrupt* checks
`preempt_count` and `TIF_NEED_RESCHED`, allowing the kernel to switch tasks
without waiting to return to user space. The exception-exit path is therefore
the universal hub where the scheduler can step in.

---

## 14. Nesting, Masking (DAIF) & Safety

- **Entry masks everything.** Hardware sets D, A, I, F on entry, so a handler
  begins non-reentrant. Linux re-enables IRQs (`I`) only after it has saved
  state and is on a safe stack.
- **Nesting.** A higher-priority interrupt can interrupt a lower one once
  unmasked; the second exception reuses `ELR_EL1`/`SPSR_EL1`, so Linux must save
  them into `pt_regs` *before* unmasking — otherwise the inner exception would
  clobber the outer return state. This is exactly why `kernel_entry` stashes
  `elr`/`spsr` into the frame immediately.
- **`EL1t` "should never happen".** Linux runs the kernel as `EL1h`. The `EL1t`
  vectors (0x000–0x180) are not expected to fire; their stubs lead to a
  `bad_mode` / `__bad_stack` panic, which catches stack corruption.
- **Stack overflow guard.** A dedicated overflow stack + guard checks detect a
  blown kernel stack and report it instead of silently corrupting memory.
- **DAIF discipline.** `local_irq_disable()/enable()` map to setting/clearing the
  `I` bit; `local_daif_save/restore()` manage the whole set during critical
  entry/exit windows.

---

## 15. Quick-Reference Cheat Sheets

### 15.1 Key registers

| Register | Role |
|----------|------|
| `VBAR_EL1` | Base address of the EL1 vector table |
| `ELR_EL1` | Return PC (restored by `ERET`) |
| `SPSR_EL1` | Saved PSTATE (restored by `ERET`) |
| `ESR_EL1` | Syndrome: `EC` (what) + `ISS` (details) |
| `FAR_EL1` | Faulting virtual address (aborts) |
| `SP_EL0` / `SP_EL1` | Thread (user) SP / handler (kernel) SP |
| `PSTATE.DAIF` | Debug / SError / IRQ / FIQ masks |
| `TTBR0/TTBR1_EL1` | User / kernel translation table bases (KPTI swaps these) |

### 15.2 Vector table offsets (from `VBAR_EL1`)

| Offset | Source | Class |
|--------|--------|-------|
| 0x000 / 0x080 / 0x100 / 0x180 | Current EL, SP_EL0 (EL1t) | Sync / IRQ / FIQ / SError |
| 0x200 / 0x280 / 0x300 / 0x380 | Current EL, SP_ELx (EL1h) — kernel | Sync / IRQ / FIQ / SError |
| 0x400 / 0x480 / 0x500 / 0x580 | Lower EL, AArch64 (EL0 64-bit) | Sync / IRQ / FIQ / SError |
| 0x600 / 0x680 / 0x700 / 0x780 | Lower EL, AArch32 (EL0 32-bit) | Sync / IRQ / FIQ / SError |

### 15.3 Common `ESR_EL1.EC` codes

| EC | Symbol | Meaning |
|----|--------|---------|
| 0x15 | `ESR_ELx_EC_SVC64` | System call (`svc #0`) |
| 0x24 / 0x25 | `ESR_ELx_EC_DABT_LOW` / `_CUR` | Data abort (user / kernel) |
| 0x20 / 0x21 | `ESR_ELx_EC_IABT_LOW` / `_CUR` | Instruction abort |
| 0x07 | FP/SIMD/SVE trap | Lazy FP state restore |
| 0x3C | `ESR_ELx_EC_BRK64` | Software breakpoint (`BRK`) |
| 0x00 | `ESR_ELx_EC_UNKNOWN` | Undefined instruction |

### 15.4 Syscall ABI (AArch64)

| Register | Purpose |
|----------|---------|
| `x8` | Syscall number |
| `x0`–`x5` | Arguments 1–6 |
| `x0` | Return value (or `-errno`) |
| `svc #0` | The trap instruction |

### 15.5 Source map (mainline ~v6.x)

| File | What lives there |
|------|------------------|
| `arch/arm64/kernel/entry.S` | `vectors`, `kernel_entry`, `kernel_exit`, `ret_to_user` |
| `arch/arm64/kernel/entry-common.c` | `el0t_64_*_handler`, `el1h_64_*_handler`, enter/exit bookkeeping |
| `arch/arm64/kernel/syscall.c` | `el0_svc`, `invoke_syscall` |
| `arch/arm64/mm/fault.c` | `do_mem_abort`, `do_page_fault` |
| `arch/arm64/kernel/traps.c` | `do_serror`, undefined-instruction handling |
| `arch/arm64/include/asm/ptrace.h` | `struct pt_regs` |
| `arch/arm64/include/asm/esr.h` | `ESR_ELx_EC_*` definitions |
| `drivers/irqchip/irq-gic-v3.c` | `gic_handle_irq` (GICv3) |

---

## 16. Summary — Why / When / Where / How in One Breath

- **Why?** The CPU needs a guarded, atomic way to escalate privilege (EL0→EL1)
  and hand control to *kernel-chosen* code. Exceptions are that one gate; they
  make process isolation, virtual memory, and multitasking possible.
- **When?** On any synchronous trap (`SVC` syscall, page-fault abort, undefined
  instruction), or any asynchronous event (IRQ, FIQ, SError).
- **Where?** To EL1 (the kernel), via `VBAR_EL1 + offset` — one of 16 vector
  slots chosen by *source* (EL0/EL1, AArch64/32, thread/handler SP) and *class*
  (Sync/IRQ/FIQ/SError).
- **How?** Hardware saves `ELR/SPSR/ESR/FAR`, masks DAIF, raises the EL, and
  jumps to the vector. `kernel_entry` builds `pt_regs` on the kernel stack, a C
  handler decodes `ESR.EC` and does the work, then `ret_to_user` + `kernel_exit`
  restore state and `ERET` returns to EL0 — resuming exactly where execution
  left off.

> A system call is simply the *best-behaved* member of this family: a
> synchronous exception (`EC = SVC64`) deliberately raised by user code. Page
> faults, interrupts, and hardware errors all travel the very same rails.
