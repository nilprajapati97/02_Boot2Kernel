# `init_cpu_task` — Stack, Per-CPU, and SCS Setup

**Source:** `arch/arm64/kernel/head.S` lines 195–213

## Purpose

The `init_cpu_task` macro transforms the CPU from "raw assembly state" to "ready for C function calls" by setting up the stack pointer, current task pointer, shadow call stack, and per-CPU data offset.

## What It Does

### 1. Set Current Task Pointer

```asm
msr  sp_el0, \tsk    // SP_EL0 = &init_task
```

ARM64 Linux uses `SP_EL0` (the user-mode stack pointer, unused in kernel mode) to store the **current task pointer**. The `current` macro reads this register:

```c
// arch/arm64/include/asm/current.h
static inline struct task_struct *get_current(void)
{
    return (struct task_struct *)read_sysreg(sp_el0);
}
#define current get_current()
```

During boot, `current` = `init_task` (PID 0, the swapper/idle task).

### 2. Set Kernel Stack Pointer

```asm
ldr     \tmp1, [\tsk, #TSK_STACK]    // task->stack
add     sp, \tmp1, #THREAD_SIZE       // stack top
sub     sp, sp, #PT_REGS_SIZE         // reserve pt_regs space
```

Each task has its own kernel stack. The stack grows downward. `PT_REGS_SIZE` bytes are reserved at the top for the `pt_regs` structure that gets filled when entering the kernel from an exception.

### 3. Create Terminal Stack Frame

```asm
stp     xzr, xzr, [sp, #S_STACKFRAME]
mov     \tmp1, #FRAME_META_TYPE_FINAL
str     \tmp1, [sp, #S_STACKFRAME_TYPE]
add     x29, sp, #S_STACKFRAME
```

This creates a stack frame with `FP = 0` and `LR = 0`, marked as `FRAME_META_TYPE_FINAL`. Stack unwinders (for backtraces, perf) will stop here, producing clean stack traces.

### 4. Shadow Call Stack (SCS)

```asm
scs_load_current
```

If SCS is enabled (`CONFIG_SHADOW_CALL_STACK`), this loads the shadow call stack pointer from the task's `scs_sp` field into register `x18`. The SCS stores return addresses separately from the main stack, providing return-address protection against stack buffer overflows.

### 5. Per-CPU Offset

```asm
adr_l   \tmp1, __per_cpu_offset
ldr     w\tmp2, [\tsk, #TSK_TI_CPU]     // task->thread_info.cpu = 0
ldr     \tmp1, [\tmp1, \tmp2, lsl #3]    // __per_cpu_offset[0]
set_this_cpu_offset \tmp1                 // TPIDR_EL1 = offset
```

Per-CPU variables are accessed by adding a CPU-specific offset to a base address. This offset is stored in `TPIDR_EL1` (a system register) for fast access:

```c
// Per-CPU access pattern:
per_cpu_var_addr = &__per_cpu_start + __per_cpu_offset[cpu_id] + var_offset
```

## `init_task` — The First Task

`init_task` is statically defined in `init/init_task.c`:
- PID 0
- Name: "swapper"
- Stack: `init_stack` (statically allocated, `THREAD_SIZE` bytes)
- Never scheduled on a run queue — becomes the idle task for CPU 0
- All other tasks are forked from `init_task`

## Key Takeaway

`init_cpu_task` is the minimal setup for running C code: it provides a stack, identifies the current task, enables per-CPU data access, and sets up return-address protection. After this macro, the CPU can call C functions, access `current`, and use per-CPU variables.
