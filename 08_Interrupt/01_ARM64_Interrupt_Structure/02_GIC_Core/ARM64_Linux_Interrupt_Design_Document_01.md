# ARMv8 (ARM64) Exception Levels

ARMv8-A defines **four Exception Levels (EL0–EL3)** that form a hierarchical privilege model. Higher numbers = higher privilege.

## The Four Levels

| Level | Privilege | Typical Software | Purpose |
|-------|-----------|------------------|---------|
| **EL0** | Lowest (unprivileged) | User applications | Normal app execution; no access to system registers |
| **EL1** | Privileged (kernel) | OS kernel (Linux, Windows) | Memory management, scheduling, device drivers |
| **EL2** | Hypervisor | KVM, Xen, Hyper-V | Virtualization; manages guest OSes |
| **EL3** | Secure Monitor (highest) | ARM Trusted Firmware (ATF/TF-A), Secure Monitor | Switches between Secure and Non-secure worlds |

## Key Properties

- **Exceptions raise the EL** (e.g., syscall from EL0 → EL1; hypercall from EL1 → EL2). Returning via `ERET` lowers it.
- **Each EL (except EL0) has its own banked system registers**: `SP_ELx`, `ELR_ELx`, `SPSR_ELx`, `VBAR_ELx`, `TTBRx_ELy`, `SCTLR_ELx`, etc.
- **EL0 has no vector table of its own** — exceptions taken from EL0 are handled at EL1 (or higher).
- **EL2 and EL3 are optional** in an implementation; EL1 and EL0 are mandatory.
- **Execution state** (AArch64 vs AArch32) can differ per EL, but a lower EL cannot use a wider state than a higher EL.

## TrustZone (Orthogonal Axis)

ARMv8 also splits execution into **Secure** and **Non-secure** worlds:

```
                 Non-secure              Secure
        ┌─────────────────────┬──────────────────────┐
  EL0   │  User apps          │  Trusted apps (TAs)  │
  EL1   │  Linux kernel       │  Trusted OS (OP-TEE) │
  EL2   │  Hypervisor (KVM)   │  Secure hypervisor*  │
  EL3   │           Secure Monitor (TF-A)            │
        └────────────────────────────────────────────┘
```

EL3 is always Secure and mediates world switches via the `SMC` instruction.

## Typical Boot Flow (ARM64 Linux)

1. **BL1 / BL2** boot ROM → runs at **EL3**
2. **BL31** (Secure Monitor / TF-A) → installed at **EL3**
3. **BL32** (OP-TEE, optional) → **Secure EL1**
4. **BL33** (U-Boot / UEFI) → **Non-secure EL2**
5. **Linux kernel** → drops to **EL1** (or stays at EL2 for KVM host)
6. **User space** → **EL0**

## How Exceptions Change EL

When an exception is taken to `ELx`:
- `ELR_ELx` ← return address
- `SPSR_ELx` ← saved `PSTATE` (includes previous EL)
- `PC` ← `VBAR_ELx` + offset (based on source EL/SP and exception type)
- Execution continues at `ELx`

`ERET` restores `PSTATE` from `SPSR_ELx` and `PC` from `ELR_ELx`, returning to the original EL.

## Instructions That Cross ELs

| Instruction | From → To |
|-------------|-----------|
| `SVC` (Supervisor Call) | EL0 → EL1 (Linux syscall) |
| `HVC` (Hypervisor Call) | EL1 → EL2 (KVM hypercall) |
| `SMC` (Secure Monitor Call) | EL1/EL2 → EL3 (PSCI, firmware services) |
| `ERET` | ELx → lower EL |

## Relevance to Your Document

Since you're working on ARM64_Linux_Interrupt_Design_Document.md, key interrupt-handling implications:

- **IRQs/FIQs** are routed per `SCR_EL3` (Secure routing), `HCR_EL2` (virtualization routing), and `PSTATE.DAIF` masks.
- Linux runs at **EL1** (or **EL2** as KVM host with VHE), so the IRQ vector lives in **`VBAR_EL1`** (or `VBAR_EL2`).
- The GIC (Generic Interrupt Controller) delivers interrupts that ultimately trap to the EL configured by `HCR_EL2.IMO/FMO` and `SCR_EL3.IRQ/FIQ`.
- On exception entry, the CPU auto-switches to `SP_ELx` (per-EL stack pointer) — Linux uses this for its per-CPU IRQ stack setup.

Want me to expand on any particular aspect (vector tables, DAIF masking, GIC routing, or VHE)?


# SP_ELx and Linux's Per-CPU IRQ Stack

## The Hardware Mechanism

ARM64 provides **per-EL banked stack pointers**: `SP_EL0`, `SP_EL1`, `SP_EL2`, `SP_EL3`. Additionally, each EL (≥1) can choose between two stack pointers via the `PSTATE.SP` bit:

| `SPSel` value | Stack used at ELx | Mode name |
|---------------|-------------------|-----------|
| `0` | `SP_EL0` (shared with user) | **`ELx_t`** (thread) |
| `1` | `SP_ELx` (EL's own SP) | **`ELx_h`** (handler) |

### What the CPU does on exception entry

When an exception is taken **to ELx**:

1. `PSTATE` is saved into `SPSR_ELx`.
2. Return address goes into `ELR_ELx`.
3. **`SPSel` is forced to `1`** → the CPU automatically starts using `SP_ELx`.
4. PC jumps to the appropriate slot in `VBAR_ELx`.

So whatever value was previously loaded into `SP_EL1` becomes the active stack the instant the handler starts — no software switch, no memory load, zero instructions.

This is the *hardware hook* Linux exploits.

## How the Vector Table Encodes Stack Choice

`VBAR_EL1` points to a 2KB table with **16 entries × 128 bytes**, grouped by *where the exception came from*:

```
Offset   Source of exception              Stack in use on entry
──────   ────────────────────────────     ─────────────────────
0x000    Current EL with SP_EL0  (EL1t)   SP_EL0  ← kernel was on task stack
0x200    Current EL with SP_ELx  (EL1h)   SP_EL1  ← kernel was on IRQ/handler stack
0x400    Lower EL using AArch64  (EL0)    SP_EL0  → switched to SP_EL1
0x600    Lower EL using AArch32           (legacy)
```

Each 128-byte block has 4 sub-entries: **Synchronous, IRQ, FIQ, SError**.

## Linux's Strategy

Linux configures things so that:

- **Kernel threads** run at EL1 with `SPSel = 1`, using `SP_EL1` = the **task's kernel stack** (`task->stack`, ~16 KB `THREAD_SIZE`).
- **User threads** run at EL0 using `SP_EL0` = the **user stack**.
- On entry from EL0, the CPU auto-switches to `SP_EL1` (the task's kernel stack) — instantly safe.
- On entry from EL1 (nested exception), the kernel is *already* on a kernel stack.

### The per-CPU IRQ stack twist

Historically Linux handled IRQs on the interrupted task's kernel stack. Modern arm64 Linux uses a **dedicated per-CPU IRQ stack** to:

- Bound worst-case stack depth (IRQ + task usage don't compound).
- Improve cache locality for hot IRQ paths.
- Mirror x86's behavior.

The per-CPU IRQ stack is allocated as:

```c
DEFINE_PER_CPU(unsigned long [IRQ_STACK_SIZE/sizeof(long)], irq_stack)
    __aligned(16);
```

And the switch happens in [arch/arm64/kernel/entry.S](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/entry.S) via the macro `irq_stack_entry` / helper `call_on_irq_stack`:

```asm
// Simplified flow when an IRQ is taken from EL0 or EL1:
//
//   1. HW vectored to VBAR_EL1 + offset; SP is now SP_EL1 (task kernel stack)
//   2. kernel_entry saves all GP regs into pt_regs on the task stack
//   3. call_on_irq_stack:
//        - load this_cpu's irq_stack_ptr
//        - save current SP into x29 (frame record)
//        - mov sp, <irq_stack_ptr>
//        - bl  handle_arch_irq      // runs on IRQ stack
//        - mov sp, x29              // restore task stack
//   4. kernel_exit restores regs and ERETs
```

Key points:

- The hardware's `SP_EL1` switch gets us onto a **valid kernel stack** (the task's). 
- Software then performs a **second switch** to the per-CPU IRQ stack only for the duration of the handler body.
- `pt_regs` lives on the **task stack**, not the IRQ stack — so unwinders and `current_pt_regs()` still work.

## Why the Two-Stage Switch?

You might ask: why not just point `SP_EL1` directly at the IRQ stack?

Because `SP_EL1` must be valid for **every** EL1 entry — synchronous exceptions (page faults, syscalls via SVC), SError, debug traps — not only IRQs. Those need the **task's** kernel stack to:

- Build `pt_regs` describing the interrupted context.
- Allow normal kernel code (which may sleep, take faults) to run.
- Be preempted/scheduled.

The IRQ stack is a **transient, non-sleeping** context — perfect for being switched to *after* entry bookkeeping.

## `SP_EL0` as the `current` Pointer (arm64-specific trick)

Linux arm64 repurposes the unused `SP_EL0` while in kernel mode to hold the `task_struct *current` pointer. On kernel entry from EL0, the user SP is saved into `pt_regs`, then `SP_EL0` is overwritten with `current`. This makes `current` a single-register read with no memory access:

```c
static __always_inline struct task_struct *get_current(void)
{
    unsigned long sp_el0;
    asm ("mrs %0, sp_el0" : "=r" (sp_el0));
    return (struct task_struct *)sp_el0;
}
```

See [arch/arm64/include/asm/current.h](https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/current.h).

## Summary Diagram

```
        EL0 (user task)
        ┌──────────────────┐
        │  user code       │   SP_EL0 = user stack
        └────────┬─────────┘
                 │  IRQ
                 ▼
        ┌──────────────────┐
        │  vector @ 0x480  │   HW: SPSel←1, SP=SP_EL1=task kernel stack
        │  kernel_entry    │   build pt_regs on task stack
        │  ┌────────────┐  │
        │  │ switch to  │  │   sw: SP ← per-CPU irq_stack
        │  │ IRQ stack  │  │
        │  │ handle_arch_irq
        │  │ switch back│  │   sw: SP ← task kernel stack
        │  └────────────┘  │
        │  kernel_exit     │
        │  ERET            │   HW: restore PSTATE, PC ← ELR_EL1
        └────────┬─────────┘
                 ▼
              back to EL0
```

