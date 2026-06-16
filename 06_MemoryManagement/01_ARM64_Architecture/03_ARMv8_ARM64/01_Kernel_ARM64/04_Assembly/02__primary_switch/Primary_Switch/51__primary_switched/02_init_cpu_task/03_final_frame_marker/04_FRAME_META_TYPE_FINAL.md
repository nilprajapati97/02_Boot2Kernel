# `FRAME_META_TYPE_FINAL` — The Unwind Terminator

## Type System for Frame Records

Linux ARM64 stack frames carry type metadata to help the unwinder understand
what kind of frame it's looking at:

```c
// arch/arm64/include/asm/stacktrace/frame.h

/* Frame meta types */
enum frame_meta_type {
    FRAME_META_TYPE_UNKNOWN  = 0,  // default / uninitialized
    FRAME_META_TYPE_FINAL    = 1,  // bottom of stack — stop unwinding
    FRAME_META_TYPE_PT_REGS  = 2,  // pt_regs context (exception boundary)
};
```

`init_cpu_task` sets `FRAME_META_TYPE_FINAL = 1` to mark the boot frame as the
definitive bottom of the kernel call stack.

---

## Why Was This Type System Added?

Before the explicit type system, the unwinder relied solely on `fp == 0` to stop.
Problems with that approach:

1. **False positives:** In some exception entry paths, the saved fp could legitimately
   be 0 (if the exception was taken from a function with `fp=0`). The unwinder
   would stop prematurely, missing legitimate frames above.

2. **pt_regs crossing:** When unwinding across an exception boundary (e.g., showing
   a trace that includes the interrupted context), the unwinder needs to know there's
   a full `pt_regs` struct to extract registers from — a plain `fp=0` doesn't convey
   this information.

3. **Debugging complexity:** Without explicit types, it's harder to understand WHY
   the unwinder stopped. A type field makes it unambiguous.

The type system (added around Linux 5.15-6.0) makes the unwinder more robust and
enables richer stack trace output.

---

## How the Unwinder Uses `FRAME_META_TYPE_FINAL`

```c
// arch/arm64/kernel/stacktrace.c (simplified/conceptual):

static int notrace unwind_next_frame(struct unwind_state *state)
{
    unsigned long fp = state->fp;
    struct frame_record *record;

    // Validate fp alignment
    if (fp & 0xf)
        return -EINVAL;

    // Validate fp is on a valid kernel stack
    if (!is_on_stack(fp, ...))
        return -EINVAL;

    record = (struct frame_record *)fp;

    // Check type FIRST
    switch (record->type) {
    case FRAME_META_TYPE_FINAL:
        return -ENOENT;   // Normal termination

    case FRAME_META_TYPE_PT_REGS:
        // Recover full register context
        state->pt_regs = container_of(record, struct pt_regs, stackframe);
        // Update state from pt_regs
        state->fp = state->pt_regs->regs[29];
        state->pc = state->pt_regs->pc;
        return 0;

    default:
        // Standard frame: follow fp chain
        state->fp = record->fp;
        state->pc = record->pc;
        return 0;
    }
}
```

When `FRAME_META_TYPE_FINAL` is seen, `unwind_next_frame` returns `-ENOENT` (no more
frames). The calling loop interprets this as successful unwind termination.

---

## FINAL vs NULL Frame Pointer

Two approaches that BOTH work:

| Approach | Mechanism | Reliability |
|---|---|---|
| `fp = 0` | Unwinder checks `if (record->fp == 0) stop` | Good — industry standard |
| `type = FINAL` | Unwinder checks type field first | Better — explicit and unambiguous |

Linux uses BOTH for robustness:
```c
// Both conditions trigger termination:
if (record->type == FRAME_META_TYPE_FINAL || record->fp == 0)
    return -ENOENT;
```

The `fp=0` check is the legacy mechanism (works on old kernels too). The type
check is the modern, robust mechanism. `init_cpu_task` sets BOTH.

---

## Production Impact — Stack Traces During Kernel Oops

On a production Android or embedded Linux system, when a kernel Oops occurs:

```
[   35.123456] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000008
[   35.123457] Mem abort info:
[   35.123458]   ESR = 0x0000000096000006
...
[   35.123470] Call trace:
[   35.123471]  bad_driver_probe+0x54/0x100
[   35.123472]  really_probe+0x1e4/0x3b0
[   35.123473]  driver_probe_device+0x78/0x180
[   35.123474]  bus_for_each_drv+0xbc/0x100
[   35.123475]  device_attach+0x8c/0x100
[   35.123476]  bus_probe_device+0x38/0xb0
[   35.123477]  device_add+0x428/0x690
[   35.123478]  platform_device_add+0x1a8/0x280
[   35.123479]  devm_platform_device_register_full+0x74/0x100
[   35.123480]  init_drivers+0x88/0x100
[   35.123481]  do_one_initcall+0x64/0x1c0
[   35.123482]  kernel_init_freeable+0x1e4/0x2c4
[   35.123483]  kernel_init+0x24/0x130
[   35.123484]  ret_from_fork+0x10/0x20
```

The last entry (`ret_from_fork`) indicates the kernel thread start. The unwinder
would eventually reach `start_kernel` → boot frame sentinel → STOP. This is a
perfect stack trace because `init_cpu_task` set up the correct termination.

Without the sentinel: the trace would continue with garbage frames after `start_kernel`.

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