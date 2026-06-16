# `init_cpu_task` Macro — Full Assembly Expansion

## Macro Definition with Parameters

```asm
.macro init_cpu_task, tsk, tmp1, tmp2
    //---- OPERATION 1: current task ----
    msr     sp_el0, \tsk

    //---- OPERATION 2: stack switch ----
    ldr     \tmp1, [\tsk, #TSK_STACK]
    add     sp, \tmp1, #THREAD_SIZE
    sub     sp, sp, #PT_REGS_SIZE

    //---- OPERATION 3: frame sentinel ----
    stp     xzr, xzr, [sp, #S_STACKFRAME]
    mov     \tmp1, #FRAME_META_TYPE_FINAL
    str     \tmp1, [sp, #S_STACKFRAME_TYPE]
    add     x29, sp, #S_STACKFRAME

    //---- OPERATION 4: shadow call stack ----
    scs_load_current                // expands when CONFIG_SHADOW_CALL_STACK=y

    //---- OPERATION 5: per-CPU offset ----
    adr_l   \tmp1, __per_cpu_offset
    ldr     w\tmp2, [\tsk, #TSK_TI_CPU]
    ldr     \tmp1, [\tmp1, \tmp2, lsl #3]
    set_this_cpu_offset \tmp1
.endm
```

## Concrete Expansion at `__primary_switched` Call Site

With `tsk=x4, tmp1=x5, tmp2=x6`, the macro expands to:

```asm
// OP1
msr     sp_el0, x4

// OP2a — load init_task.stack into x5
ldr     x5, [x4, #24]              // TSK_STACK = 24 (example, build-time generated)

// OP2b — compute top of 16KB stack
add     sp, x5, #16384              // THREAD_SIZE = 16384

// OP2c — reserve pt_regs at top
sub     sp, sp, #336               // PT_REGS_SIZE = 336

// OP3a — zero out stackframe.fp and stackframe.pc
stp     xzr, xzr, [sp, #288]       // S_STACKFRAME = 288

// OP3b — mark as FINAL frame type
mov     x5, #1                      // FRAME_META_TYPE_FINAL = 1
str     x5, [sp, #304]             // S_STACKFRAME_TYPE = 288 + 16 = 304

// OP3c — set frame pointer into the pt_regs stackframe
add     x29, sp, #288              // x29 = sp + S_STACKFRAME

// OP4 — shadow call stack (if CONFIG_SHADOW_CALL_STACK=y)
mrs     x18, sp_el0                // x18 = &init_task (just set above)
ldr     x18, [x18, #TSK_TI_SCS_SP] // x18 = init_task.thread_info.scs_sp

// OP5a — get address of __per_cpu_offset array
adrp    x5, __per_cpu_offset       // PC-relative: x5 = page containing __per_cpu_offset
add     x5, x5, :lo12:__per_cpu_offset // x5 = exact VA of __per_cpu_offset

// OP5b — get CPU ID from task_struct.thread_info.cpu (zero-extends to x6)
ldr     w6, [x4, #TSK_TI_CPU]     // w6 = 0 (CPU0)

// OP5c — load __per_cpu_offset[cpu] (8-byte stride: lsl #3)
ldr     x5, [x5, x6, lsl #3]     // x5 = __per_cpu_offset[0]

// OP5d — install as per-CPU base
msr     tpidr_el1, x5             // or tpidr_el2 under VHE
```

Note: `adr_l` is a **pseudo-instruction** that expands to `adrp + add` pair.
The assembler handles the two-instruction sequence automatically.

---

## Instruction Count Summary

| Operation | Instructions (no SCS) | Instructions (with SCS) |
|---|---|---|
| OP1: current task | 1 | 1 |
| OP2: stack switch | 3 | 3 |
| OP3: frame sentinel | 4 | 4 |
| OP4: shadow call stack | 0 | 2 |
| OP5: per-CPU offset | 4 | 4 |
| **TOTAL** | **12** | **14** |

---

## Register Clobber Analysis

After the macro completes:

| Register | Original Value | Value After Macro |
|---|---|---|
| `x4` (tsk) | `&init_task` | UNCHANGED (not clobbered) |
| `x5` (tmp1) | don't care | `__per_cpu_offset[0]` |
| `x6` (tmp2) | don't care | `0` (CPU ID, w6-extended) |
| `x18` | don't care | SCS pointer (or unchanged if !SCS) |
| `x29` | `0` | `&init_task_stack_frame` |
| `sp` | early boot stack | `init_stack + 16384 - 336` |
| `sp_el0` | undefined | `&init_task` |
| `tpidr_el1` | undefined | `__per_cpu_offset[0]` |

Registers x0-x3, x7-x17, x19-x28 are NOT touched by the macro.

---

## `set_this_cpu_offset` Macro Expansion

```asm
// arch/arm64/include/asm/percpu.h
.macro set_this_cpu_offset, val
#ifdef CONFIG_ARM64_VHE
    // Under VHE, kernel runs at EL2, so use tpidr_el2
    msr     tpidr_el2, \val
#else
    msr     tpidr_el1, \val
#endif
.endm
```

---

## `scs_load_current` Macro Expansion

```asm
// arch/arm64/include/asm/scs.h
.macro scs_load_current
#ifdef CONFIG_SHADOW_CALL_STACK
    get_current_task x18           // mrs x18, sp_el0
    ldr x18, [x18, #TSK_TI_SCS_SP]
#endif
.endm

.macro get_current_task, tsk
    mrs     \tsk, sp_el0
.endm
```

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