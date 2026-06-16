# init_cpu_task — CPU Perspective: Register Banking, SP_EL0, and Per-CPU State

**Classification**: ARM64 CPU Microarchitecture — Primary Boot Path
**Scope**: `init_cpu_task` macro in `__primary_switched`
**Perspective**: CPU register file, exception level banking, system registers
**Style Reference**: AMD64 Architecture Programmer's Manual / NVIDIA Tegra TRM

---

## 1. ARM64 Stack Pointer Banking — Hardware Architecture

ARM64 implements **four independent stack pointer registers**, one per exception level:

```
 ┌─────────────────────────────────────────────────────────────────────────┐
 │                   ARM64 Exception Level Stack Pointer Bank              │
 ├──────────────┬──────────────────┬─────────────────────────────────────┤
 │  Register    │  Exception Level │  Linux Usage                        │
 ├──────────────┼──────────────────┼─────────────────────────────────────┤
 │  SP_EL0      │  EL0 (userspace) │  Repurposed: `current` task pointer │
 │  SP_EL1      │  EL1 (kernel)    │  Active kernel stack pointer        │
 │  SP_EL2      │  EL2 (hypervisor)│  Hypervisor stack / VHE: SP_EL1    │
 │  SP_EL3      │  EL3 (secure)    │  Secure Monitor stack               │
 └──────────────┴──────────────────┴─────────────────────────────────────┘
```

At EL1, the CPU selects the active stack pointer via `PSTATE.SP` (SPSel bit):

```
PSTATE.SP = 0  →  active SP = SP_EL0   (EL0t mode)
PSTATE.SP = 1  →  active SP = SP_EL1   (EL1h mode)  ← Linux runs here
```

After `init_kernel_el` sets `INIT_PSTATE_EL1`, `PSTATE.SP = 1`.
Linux always runs EL1 with `SPSel=1`, so `SP_EL1` is the real stack.
**`SP_EL0` is architecturally idle at EL1** — the hardware never reads it
for instruction execution. Linux exploits this "dead register" as a CPU-local
pointer to the current `task_struct`.

---

## 2. The `msr sp_el0, x4` Instruction — Binding CPU to `init_task`

```asm
adr_l   x4, init_task       // x4 = virtual address of init_task (the idle task)
msr     sp_el0, x4          // SP_EL0 = &init_task
```

This single instruction establishes the **CPU–task binding** for CPU0.

### Hardware Semantics

`msr sp_el0, x4` at EL1 writes x4 into the banked SP_EL0 register.
Because `PSTATE.SP = 1`, this write does NOT change the active stack pointer.
The CPU continues executing from SP_EL1. SP_EL0 just silently holds the new
value.

### Why This Works as `current`

Every reference to `current` in C compiles to:

```c
// arch/arm64/include/asm/current.h
static __always_inline struct task_struct *get_current(void)
{
    unsigned long sp_el0;
    asm("mrs %0, sp_el0" : "=r" (sp_el0));
    return (struct task_struct *)sp_el0;
}
```

**Cost**: One `mrs` instruction (system register read). This is **1 cycle** —
no cache lookup, no memory access, no TLB miss possible. It is the fastest
possible implementation of a CPU-local variable in ARM64.

Compare against alternatives:
```
Approach                       Cost
─────────────────────────────────────────────────────
SP_EL0 (Linux choice)          1 cycle (MRS)
Per-CPU array [current_task]   1 MRS (TPIDR_EL1) + 1 load + 1 pointer deref
Thread-local via FS/GS (x86)   1 load from segment base
Stack base calculation         1 AND + 1 load (stack pointer masking)
```

ARM64's design with four banked SP registers creates a "free" register for
exactly this purpose. Linux's use of SP_EL0 as `current` is one of the
cleanest CPU-ABI co-designs in the kernel.

---

## 3. Stack Pointer Setup — Migrating from `early_init_stack` to `init_task` Stack

Before `init_cpu_task`, `sp` points to `early_init_stack` (4KB, in BSS,
established in `primary_entry`, used throughout the pre-MMU boot path).

`init_cpu_task` migrates to the proper `init_task` kernel stack:

```asm
ldr    tmp1, [x4, #TSK_STACK]      // tmp1 = init_task.stack (base addr)
add    sp,   tmp1, #THREAD_SIZE    // sp = base + THREAD_SIZE (top of stack)
sub    sp,   sp,   #PT_REGS_SIZE   // Reserve PT_REGS_SIZE bytes at top
```

### Why Reserve `PT_REGS_SIZE` at the Top?

```
Virtual address (high)
┌─────────────────────────────────┐  ← init_task.stack + THREAD_SIZE
│         pt_regs region          │  ← RESERVED HERE by sub sp, sp, #PT_REGS_SIZE
│   (holds stackframe sentinel)   │  ← sp points here after init_cpu_task
├─────────────────────────────────┤
│                                 │
│      kernel stack grows down    │  ← future frames pushed here by start_kernel
│                                 │
│                                 │
└─────────────────────────────────┘  ← init_task.stack (low address)
Virtual address (low)
```

The `pt_regs` region at the top of every task's stack is an ARM64 invariant:
`task_pt_regs(task)` returns `(struct pt_regs *)(task->stack + THREAD_SIZE) - 1`,
which is exactly the region reserved here. Placing `pt_regs` at a **fixed
known offset** from the top of the stack allows the unwinder to always find it:
"to find the bottom frame record, go to `task->stack + THREAD_SIZE - sizeof(pt_regs)`".

---

## 4. TPIDR_EL1 — Per-CPU Variable Base

```asm
adr_l   tmp1, __per_cpu_offset          // Address of per-CPU offset array
ldr     w_tmp2, [x4, #TSK_TI_CPU]       // w_tmp2 = init_task.cpu = 0
ldr     tmp1, [tmp1, tmp2, lsl #3]      // tmp1 = __per_cpu_offset[cpu0]
msr     tpidr_el1, tmp1                 // Install into TPIDR_EL1
```

### ARM64 System Register: TPIDR_EL1

```
TPIDR_EL1: Thread ID Register, EL1
┌─────────────────────────────────────────────────────┐
│                      [63:0]                         │
│                 Software-Defined Value              │
│         (ARM architecture imposes no semantics)     │
└─────────────────────────────────────────────────────┘
Access: EL1 read/write (EL0 has no access)
Reset:  IMPLEMENTATION DEFINED (do not rely on boot value)
```

Linux repurposes TPIDR_EL1 as the **per-CPU data segment offset**.
The per-CPU variable infrastructure works as follows:

```
Physical layout of per-CPU data:
  [prototype section]          ← defined by linker at link time
  [CPU0 copy]  at offset __per_cpu_offset[0]
  [CPU1 copy]  at offset __per_cpu_offset[1]
  ...
  [CPUN copy]  at offset __per_cpu_offset[N]

TPIDR_EL1 on CPU X = __per_cpu_offset[X]

this_cpu_ptr(ptr) = (typeof(ptr))((ulong)ptr + TPIDR_EL1)
```

### Generated Assembly for `this_cpu_read(var)`

```asm
// C: int x = this_cpu_read(my_counter);
mrs   x0, tpidr_el1               // Load per-CPU offset (1 cycle)
adrp  x1, my_counter              // Load prototype address (1 cycle)
add   x1, x1, :lo12:my_counter    // Complete prototype address
ldr   w0, [x1, x0]                // Load from [prototype + per_cpu_offset]
```

Total: 4 instructions, 0 cache misses if per-CPU data is hot in L1D.

### Ordering Requirement

`msr tpidr_el1` has **immediate visibility** — subsequent `mrs tpidr_el1`
on the same CPU will read the new value. No ISB is required because TPIDR_EL1
is a non-architectural register with no pipeline dependency.

Cross-CPU visibility of TPIDR_EL1 is not a concern — each CPU has its own
physical instance of the register, and per-CPU data is by definition
not shared across CPUs without explicit synchronization.

---

## 5. Full CPU Register State After `init_cpu_task`

```
┌────────────────────────────────────────────────────────────────────────────┐
│                 CPU Register State After init_cpu_task                     │
├──────────────────┬─────────────────────────┬──────────────────────────────┤
│  Register        │  Value                  │  Meaning                     │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  SP (= SP_EL1)   │  init_task.stack        │                              │
│                  │  + THREAD_SIZE          │  Kernel stack, post-reserve  │
│                  │  - PT_REGS_SIZE         │                              │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  SP_EL0          │  &init_task             │  current task pointer        │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  TPIDR_EL1       │  __per_cpu_offset[0]    │  CPU0 per-CPU base offset    │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  x18             │  init_task.scs_base     │  Shadow Call Stack (if SCS)  │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  x29 (FP)        │  sp + S_STACKFRAME      │  Bottom frame pointer        │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  TPIDR_EL0       │  (unchanged, zero)      │  Future: user-space TLS      │
├──────────────────┼─────────────────────────┼──────────────────────────────┤
│  VBAR_EL1        │  NOT YET SET            │  Set in next step            │
└──────────────────┴─────────────────────────┴──────────────────────────────┘
```

---

## 6. Critical Window: VBAR_EL1 Is Not Yet Valid

**NVIDIA Engineering Note**: Between `msr sp_el0, x4` (start of `init_cpu_task`)
and `msr vbar_el1, x8` (the very next instruction block in `__primary_switched`),
the CPU is in a state where:

- The stack is valid (init_task stack)
- `current` is valid (SP_EL0 = init_task)
- Per-CPU data is valid (TPIDR_EL1 set)
- **But VBAR_EL1 still contains whatever `__cpu_setup` left** (typically
  `__hyp_stub_vectors` for EL2 path, or the EL1 reset value for EL1 path)

If any exception occurs in this window — a prefetch abort, an alignment fault,
a spurious IRQ — the CPU would vector to the wrong address and the system would
either hang or silently corrupt state.

This window is intentionally **as short as possible** (the `adr_l + msr + isb`
sequence for VBAR follows immediately after `init_cpu_task`). ARM64's
precise exception model guarantees that `msr vbar_el1` + `isb` completes
before the next instruction, so the window is bounded at exactly 3 instructions:
`adr_l x8, vectors` + `msr vbar_el1, x8` + `isb`.

---

## 7. AMD-Style Ordering Analysis: What Happens If Init Order Changes?

```
Correct order (enforced by code):
  1. init_cpu_task (SP, SP_EL0, TPIDR_EL1)   — task binding
  2. msr vbar_el1                              — exception routing
  3. stp x29, x30 (frame setup)               — unwinder anchor
  4. save __fdt_pointer                        — global state
  5. compute kimage_voffset                    — VA/PA bridge

If vbar_el1 install is MOVED AFTER kimage_voffset computation:
  → Any fault during kimage_voffset computation (e.g. translation fault
    on swapper_pg_dir being stale) would vector to wrong address
  → System hangs silently with no debug output possible

If tpidr_el1 (per-CPU) is DELAYED past start_kernel:
  → boot_cpu_init() calls this_cpu_write() which dereferences TPIDR_EL1
  → If TPIDR_EL1 is 0 (reset value), this_cpu_ptr points to the prototype
    section which is read-only after mark_rodata_ro()
  → Write fault in start_kernel() with no stack trace (VBAR not valid yet)
```

This analysis demonstrates why the ordering in `__primary_switched` is not
arbitrary — it is the **minimum viable sequence** to establish a safe CPU
state before entering C code.
