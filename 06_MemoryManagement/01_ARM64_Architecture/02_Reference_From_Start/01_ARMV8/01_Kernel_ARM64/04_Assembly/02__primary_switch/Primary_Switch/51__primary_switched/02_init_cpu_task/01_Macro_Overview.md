# `init_cpu_task` Macro — Complete Overview

## What It Is

`init_cpu_task` is an **assembler macro** defined in `arch/arm64/kernel/head.S`.
It initializes every CPU-specific register and data structure needed for the C runtime
to function correctly on that CPU.

It is called in TWO places:
1. **`__primary_switched`** — for the boot (primary) CPU with `init_task` as the task
2. **`__secondary_switched`** — for each secondary CPU with their own task struct

---

## The Macro Definition

```asm
// arch/arm64/kernel/head.S
.macro  init_cpu_task tsk, tmp1, tmp2
    msr     sp_el0, \tsk                      // (1) Register task as "current"

    ldr     \tmp1, [\tsk, #TSK_STACK]         // (2a) Load stack base
    add     sp, \tmp1, #THREAD_SIZE            // (2b) SP = stack top
    sub     sp, sp, #PT_REGS_SIZE              // (2c) Reserve pt_regs at top

    stp     xzr, xzr, [sp, #S_STACKFRAME]     // (3a) fp=0, lr=0 sentinel
    mov     \tmp1, #FRAME_META_TYPE_FINAL      // (3b)
    str     \tmp1, [sp, #S_STACKFRAME_TYPE]   // (3c) mark final frame
    add     x29, sp, #S_STACKFRAME            // (3d) FP → stackframe

    scs_load_current                           // (4)  SCS pointer (x18)

    adr_l   \tmp1, __per_cpu_offset           // (5a) per-CPU array address
    ldr     w\tmp2, [\tsk, #TSK_TI_CPU]       // (5b) CPU ID from task
    ldr     \tmp1, [\tmp1, \tmp2, lsl #3]     // (5c) per-CPU base for this CPU
    set_this_cpu_offset \tmp1                 // (5d) store to tpidr_el1
.endm
```

---

## Call Site in `__primary_switched`

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    adr_l   x4, init_task     // x4 = &init_task (virtual address)
    init_cpu_task x4, x5, x6  // tsk=x4, tmp1=x5, tmp2=x6
```

**Arguments:**
- `tsk = x4` = pointer to `init_task` (PID 0, boot CPU's task)
- `tmp1 = x5` = scratch register (clobbered)
- `tmp2 = x6` = scratch register (clobbered)

---

## The Five Operations — Why Each One Is Necessary

### Operation 1: `msr sp_el0, tsk` → Register Current Task

Linux repurposes `sp_el0` (EL0 stack pointer, unused at EL1) as the fast "current"
pointer. The `current` macro expands to `mrs xN, sp_el0`.

**Without this:** Every call to `current` returns garbage. `start_kernel`'s first
line calls `set_task_stack_end_magic(current)` — would corrupt random memory.

### Operation 2: Stack Switch

Switches SP from the temporary `early_init_stack` to `init_task`'s permanent
`init_stack` (16KB). Reserves `PT_REGS_SIZE` (336 bytes) at the top for the
standardized per-task pt_regs block.

**Without this:** Stack unwinder has no final sentinel; `current->stack` points
to `early_init_stack` which has no `thread_info` at its base; per-task accounting
(stack overflow detection) would fail.

### Operation 3: Final Frame Sentinel

Stamps `{fp=0, lr=0, type=FINAL}` at `task_pt_regs(current)->stackframe`.
Sets `x29` (frame pointer) to point to this record.

**Without this:** Stack unwinder walks off the end of `init_task`'s stack into
unmapped memory → page fault during the first kernel oops/backtrace.

### Operation 4: `scs_load_current` → Shadow Call Stack

Loads `init_task.thread_info.scs_sp` into `x18` (the ABI-reserved SCS register).

**Without this (when CONFIG_SHADOW_CALL_STACK is set):** The first function return
(`ret`) after `bl set_cpu_boot_mode_flag` compares `x30` against the shadow stack
pointer at `x18` — which is garbage → mismatch → kernel panic. The entire SCS
defense system is broken from the first function call.

### Operation 5: Per-CPU Offset → `tpidr_el1`

Loads `__per_cpu_offset[init_task.thread_info.cpu]` into `tpidr_el1` (and
`tpidr_el2` under VHE). This is the per-CPU base address register.

**Without this:** Every `this_cpu_read(var)` / `this_cpu_write(var)` uses `tpidr_el1=0`
as the base → accesses memory at address `0 + var_offset` = unmapped → page fault
in the very first per-CPU variable access in `start_kernel`.

---

## Why `init_cpu_task` Must Come First in `__primary_switched`

The ordering constraint is a dependency chain, not a style choice:

```
init_cpu_task
  ↓ provides valid SP
msr vbar_el1  ← exception entry WRITES to SP (needs init_stack, not early_init_stack)
  ↓ provides safe exceptions
stp x29,x30   ← safe to push stack (exceptions handled if push faults)
  ↓ provides C ABI frame
bl set_cpu_boot_mode_flag  ← safe bl call with return address saved
```

If `msr vbar_el1` ran BEFORE `init_cpu_task`, exceptions during `init_cpu_task`
would dispatch to the valid vector table, but with SP = `early_init_stack`. The
exception handler would push state onto `early_init_stack`, but `current` would
still be garbage — the handler would then call `current` and crash.

The only safe ordering is: establish valid current+stack FIRST, THEN install VBAR.

---

## Comparison: Primary vs Secondary CPU

| | Primary CPU | Secondary CPU |
|---|---|---|
| Task arg | `init_task` (static, PID 0) | `secondary_data.task` (dynamically assigned) |
| Stack | `init_stack` (in .bss) | per-task kernel stack (kmalloc'd) |
| CPU ID | `init_task.thread_info.cpu = 0` | actual CPU ID assigned by scheduler |
| ptrauth | NOT initialized here | `ptrauth_keys_init_cpu` called after |
| Call site | `__primary_switched` | `__secondary_switched` |

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the current task (process) is identified at EL0 via TPIDR_EL0 (user thread ID) and at EL1 via SP_EL0. Linux uses SP_EL0 to store the pointer to the current task_struct. SP_EL0 is a dedicated register (not the EL0 stack pointer when running in EL1 -- at EL1, SP_ELx selects either SP_EL0 or SP_EL1 as the active stack, controlled by PSTATE.SP). When the kernel uses SP_EL0 to store the current task pointer, it is using SP_EL0 as a general-purpose register (reading/writing it with MRS/MSR SP_EL0).

### Kernel Perspective (Linux ARM64)
init_cpu_task is a per-CPU variable (or boot-time initialization) that sets up the idle task (init_task / swapper) as the current task. In __primary_switched:
  msr  sp_el0, x23        // x23 holds init_task VA, set SP_EL0 = &init_task
  ldr  x8, [x23, #TSK_TI_CPU]  // verify .cpu field
The current macro in Linux ARM64 expands to:
  mrs x0, sp_el0          // read SP_EL0 as current task_struct pointer
SP_EL0 is never spilled to the stack (it is a system register), making current() essentially a zero-cost operation.

### Memory Perspective (ARMv8 Memory Model)
task_struct for init_task lives in the .data section of the kernel image (statically allocated). Its VA is in the kernel text/data mapping (TTBR1_EL1). When SP_EL0 is set to &init_task, the memory region is already mapped and accessible. The task's stack (thread_union) is in the .init.data section and is also already mapped. After start_kernel -> sched_init(), all subsequent tasks have their task_struct allocated from slab memory in the kernel heap (also in the TTBR1_EL1 region).