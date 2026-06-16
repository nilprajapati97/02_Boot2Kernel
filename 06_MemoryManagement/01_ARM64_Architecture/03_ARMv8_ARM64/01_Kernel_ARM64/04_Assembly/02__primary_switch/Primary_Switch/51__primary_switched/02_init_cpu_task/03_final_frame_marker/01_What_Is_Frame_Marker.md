# Final Frame Marker — Terminating the Unwind Chain

## What Is a Frame Marker?

ARM64 uses a "frame record" convention for stack unwinding. Each function's stack
frame contains a record at the bottom of the frame with two 64-bit values:
```c
struct frame_record {
    u64 fp;   // frame pointer to PREVIOUS frame (x29 of caller)
    u64 lr;   // link register at function entry (return address)
};
```

The unwinder traces back through the call chain by following these records:
```
Current Frame:
    [sp + 0]         → local variables
    [sp + framesize - 16]: fp = &caller's frame_record  ← x29 points here
    [sp + framesize - 8]:  lr = return address to caller

Caller's Frame:
    ...
    [callers_sp + callerframesize - 16]: fp = &callers_caller frame_record
    ...

... until fp = 0 → STOP
```

---

## The "Final Frame" Problem at Boot

The boot call chain starts at `__primary_switched`. This function:
1. Is an assembly function — no AAPCS64 prologue
2. Has no "caller" — it was jumped to, not called
3. Does NOT push a traditional frame record

If the unwinder follows fp chains and reaches `__primary_switched`'s stack area
WITHOUT a proper `fp=0` terminator, it will continue reading memory past the
stack. This causes:
- Reading arbitrary kernel memory as if it were frame pointers
- Possible page fault in the unwinder
- Infinite loop following circular "frame" pointers
- Stack trace output with garbage addresses

**Solution:** Explicitly write a `fp=0, lr=0, type=FINAL` record and set `x29`
to point to it. This creates an artificial "bottom of stack" frame.

---

## The Four Instructions

```asm
// Step 1: Write fp=0 and lr=0 into pt_regs.stackframe
stp     xzr, xzr, [sp, #S_STACKFRAME]
//     xzr = 0 (ARM64 zero register, hardwired to 0)
//     stores: [sp+S_STACKFRAME+0] = 0 (fp)
//             [sp+S_STACKFRAME+8] = 0 (lr/pc)

// Step 2: Write type = FRAME_META_TYPE_FINAL
mov     tmp1, #FRAME_META_TYPE_FINAL
str     tmp1, [sp, #S_STACKFRAME_TYPE]
//     S_STACKFRAME_TYPE = S_STACKFRAME + 16 (type field after fp+lr)
//     Stores 1 into the type field

// Step 3: Set x29 = frame pointer to the sentinel
add     x29, sp, #S_STACKFRAME
//     x29 now points directly to the zero fp/lr pair
```

---

## Memory Layout After These Four Instructions

```
init_stack top:
...
[sp + 296]  pt_regs.pmr_save       (8 bytes) = 0
[sp + 304]  pt_regs.stackframe[0]  (8 bytes) = 0   ← S_STACKFRAME
            (fp = 0: UNWIND TERMINATES HERE)
[sp + 312]  pt_regs.stackframe[1]  (8 bytes) = 0
            (lr/pc = 0)
[sp + 320]  pt_regs.stackframe_type (4 bytes) = 1   ← S_STACKFRAME_TYPE
            (FRAME_META_TYPE_FINAL = 1)
...
[sp + 335]  END of pt_regs

x29 = sp + 304   ← x29 points to the fp=0 record
```

---

## Why `xzr` Instead of `mov tmp, #0; stp tmp, ...`?

`xzr` is the ARM64 zero register:
- Physical register number 31 when used as source
- ALWAYS reads as 0 — hardwired in silicon
- Used in `stp` as source: `stp xzr, xzr` stores two 64-bit zeros

Without `xzr`:
```asm
// Inefficient alternative:
mov     x5, #0
mov     x6, #0
stp     x5, x6, [sp, #S_STACKFRAME]   // 3 instructions
```

With `xzr`:
```asm
stp     xzr, xzr, [sp, #S_STACKFRAME]  // 1 instruction
```

`xzr` saves 2 instructions. In boot code, every instruction counts for boot time.

---

## The `FRAME_META_TYPE_FINAL` — Why Not Just `fp=0`?

The Linux ARM64 unwinder has evolved. Older versions just checked `fp == 0`.
Modern versions use the explicit type field for robustness:

```c
// arch/arm64/kernel/stacktrace.c (simplified):
static int unwind_next(struct unwind_state *state)
{
    unsigned long fp = state->fp;
    struct frame_record *record;
    
    if (fp & 0xf)  // misaligned
        return -EINVAL;
    
    record = (struct frame_record *)fp;
    
    // NEW: check explicit type field
    if (record->type == FRAME_META_TYPE_FINAL)
        return -ENOENT;   // normal termination
    
    // OLD: also check fp == 0 for compatibility
    if (record->fp == 0)
        return -ENOENT;
    
    state->fp = record->fp;
    state->pc = record->pc;
    return 0;
}
```

The type field was added to handle cases where `fp=0` could be a legitimate
value in some kernel contexts (e.g., certain entry paths). The explicit type is
unambiguous.

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