# `init_cpu_task` Overview — What the Macro Does and Why It Exists

## Purpose

`init_cpu_task` is an ARM64 assembly macro defined in
`arch/arm64/include/asm/assembler.h`. Its job is to initialize the minimum CPU
state required before any kernel C code can run. It runs in a context where:
- The MMU is ON (virtual addresses are valid)
- Caches are ON
- `sp` may still point to the early boot stack (`early_init_stack`)
- No C infrastructure exists yet

The macro bridges the gap between "MMU just turned on" and "ready for C runtime."

---

## Why a Macro, Not a Function?

Using a macro instead of an assembly function (with `bl`/`ret`) avoids the
chicken-and-egg problem:

- To call a function, you need a valid stack (for `bl` to push return address)
- To set up the valid stack, you need to run the setup code

By using a macro, the code inlines directly at the call site with no stack
involvement. The macro IS the stack setup.

---

## Where the Macro Lives

```
arch/arm64/include/asm/assembler.h
    .macro init_cpu_task, tsk, tmp1, tmp2
        msr     sp_el0, \tsk
        ldr     \tmp1, [\tsk, #TSK_STACK]
        add     sp, \tmp1, #THREAD_SIZE
        sub     sp, sp, #PT_REGS_SIZE
        stp     xzr, xzr, [sp, #S_STACKFRAME]
        mov     \tmp1, #FRAME_META_TYPE_FINAL
        str     \tmp1, [sp, #S_STACKFRAME_TYPE]
        add     x29, sp, #S_STACKFRAME
        scs_load_current
        adr_l   \tmp1, __per_cpu_offset
        ldr     w\tmp2, [\tsk, #TSK_TI_CPU]
        ldr     \tmp1, [\tmp1, \tmp2, lsl #3]
        set_this_cpu_offset \tmp1
    .endm
```

---

## Call Site in `__primary_switched`

```asm
// arch/arm64/kernel/head.S
SYM_FUNC_START_LOCAL(__primary_switched)
    adr_l   x4, init_task        // load &init_task into x4
    init_cpu_task x4, x5, x6    // MACRO CALL (inlines all 13+ instructions)
    ...
```

Parameters:
- `x4` = `tsk` — pointer to `init_task` (the boot CPU's task struct)
- `x5` = `tmp1` — scratch register (clobbered inside macro)
- `x6` = `tmp2` — scratch register (clobbered inside macro)

---

## The Five Logical Operations

| # | Assembly | Effect |
|---|---|---|
| 1 | `msr sp_el0, tsk` | Set "current task" pointer (sp_el0 = &init_task) |
| 2 | `ldr+add+sub sp` | Switch to init_stack (permanent 16KB kernel stack) |
| 3 | `stp+mov+str+add x29` | Install frame sentinel (fp=0, type=FINAL, x29=frame) |
| 4 | `scs_load_current` | Load shadow call stack pointer into x18 |
| 5 | `adr_l+ldr+ldr+msr` | Set tpidr_el1 = __per_cpu_offset[cpu] |

---

## Primary vs Secondary CPU Usage

The SAME macro runs on BOTH primary and secondary CPUs:

```asm
// PRIMARY CPU (head.S __primary_switched):
adr_l   x4, init_task
init_cpu_task x4, x5, x6

// SECONDARY CPU (head.S secondary_startup):
ldr     x4, [x21, #SECONDARY_TASK]  // task passed via PSCI x1 arg
init_cpu_task x4, x5, x6
```

The macro is CPU-agnostic — it uses the `task_struct.stack` and
`task_struct.thread_info.cpu` fields which differ per-task, giving each CPU its
own stack and per-CPU offset automatically.

---

## State Change Summary

```
BEFORE init_cpu_task:
    sp        → early_init_stack + THREAD_SIZE (temporary)
    sp_el0    → undefined (firmware garbage)
    x29       → 0 (set by primary_entry)
    tpidr_el1 → undefined
    x18       → undefined

AFTER init_cpu_task:
    sp        → init_stack + THREAD_SIZE - PT_REGS_SIZE (permanent 16KB stack)
    sp_el0    → &init_task (Linux 'current' register)
    x29       → &init_task_stack_top.stackframe (frame pointer)
    tpidr_el1 → __per_cpu_offset[0] (per-CPU base for CPU0)
    x18       → init_task.thread_info.scs_sp (shadow call stack)
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