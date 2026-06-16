# Stack Unwind Chain Mechanics — From Crash to Boot Frame

## The Full Unwind Chain Visualization

```
Kernel panic / WARN / BUG  →  show_stack()  →  unwind_frame() loop

Call stack at time of panic (example):
    BUG_ON() in driver_init_device()
        ↑ called by do_one_initcall()
            ↑ called by do_initcalls()
                ↑ called by kernel_init_freeable()
                    ↑ called by kernel_init() [kthread]
                        ↑ ... 
                            ↑ start_kernel()
                                ↑ __primary_switched boot frame [fp=0, type=FINAL]
                                    STOP
```

The unwinder produces output like:
```
[<ffffffff8012abcd>] driver_init_device+0x34/0x100
[<ffffffff8023cdef>] do_one_initcall+0x78/0x200
...
[<ffffffff80100000>] start_kernel+0x0/0x400
```

The last entry (`start_kernel`) corresponds to `x29 = boot frame sentinel`.
The unwinder stops here because `fp=0` (or `FRAME_META_TYPE_FINAL`).

---

## How the Unwinder Finds the First Frame

```c
// arch/arm64/kernel/stacktrace.c

void show_stack(struct task_struct *tsk, unsigned long *sp, const char *loglvl)
{
    struct unwind_state state;
    
    // Initialize state with current x29 and lr
    unwind_init(&state, tsk, NULL, NULL);
    //          ↓
    //  state.fp = x29 (current frame pointer)
    //  state.pc = x30 (current LR / return address)
    
    // Walk frames
    do {
        // Print current frame
        print_address(state.pc);
        
        // Advance to next frame
    } while (!unwind_next(&state));
}
```

`x29` (the frame pointer) is the key. At the time of the crash, `x29` contains
the frame pointer. The unwinder reads `*x29` to get the previous `x29`, and
`*(x29+8)` to get the return address. It continues until `*x29 == 0`.

---

## The `x29` Lifetime After `init_cpu_task`

After `init_cpu_task` sets `x29`:
```
x29 = sp + S_STACKFRAME
```

This value persists through `bl start_kernel`:
```asm
// __primary_switched continues:
bl     start_kernel

// start_kernel prologue (compiled by gcc/clang):
start_kernel:
    stp    x29, x30, [sp, #-N]!    // saves OLD x29 (boot frame sentinel)
    mov    x29, sp                   // x29 = new frame for start_kernel
    ...
```

After `start_kernel`'s prologue:
- `x29` = start_kernel's frame
- `[start_kernel_frame.fp]` = OLD x29 = boot frame sentinel address
- `[boot frame sentinel]` = {fp=0, lr=0, type=FINAL}

The chain is:
```
start_kernel x29
    └─► boot frame sentinel x29 (set by init_cpu_task)
            └─► fp=0 → STOP
```

---

## What Happens WITHOUT The Frame Sentinel

If `init_cpu_task` did NOT write the frame sentinel and did NOT set `x29`:

Option 1: `x29 = 0` (set by `primary_entry` at boot start)
- `start_kernel` saves `x29=0` as its caller frame pointer
- Unwinder from start_kernel: reads fp=0 immediately → stops OK
- BUT: stack traces from WITHIN start_kernel's callees see only ONE frame
- All frames above start_kernel are invisible

Option 2: `x29 = random garbage` (if primary_entry hadn't zeroed it)
- `start_kernel` saves garbage fp
- Unwinder follows garbage pointer → accesses arbitrary kernel VA
- If unmapped: Translation Fault in unwinder → double-fault
- If mapped: reads kernel data as frame records → garbage stack trace
- Loops forever or crashes

The explicit sentinel is the robust solution.

---

## `FRAME_META_TYPE_PT_REGS` — The Other Frame Type

For completeness, there's another frame type used during exception entry:

```c
FRAME_META_TYPE_PT_REGS = 2
```

When a kernel exception handler creates a `pt_regs` context on the stack, it sets
`stackframe_type = FRAME_META_TYPE_PT_REGS`. The unwinder detects this and:
1. Reads the FULL `pt_regs` struct to recover registers
2. Continues the unwind with the pre-exception context

This allows stack traces to cross exception boundaries:
```
current_kernel_function
    ↑ [exception boundary — type=PT_REGS here]
    ↑ interrupted code (user or kernel)
```

`init_cpu_task` sets `FRAME_META_TYPE_FINAL` (not `PT_REGS`) because the boot
frame is NOT an exception context — it's just the "bottom of the world."

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