# Five Operations of `init_cpu_task` — Deep Summary

## Operation Map

```
init_cpu_task x4, x5, x6
    │
    ├── [OP 1] msr sp_el0, x4          → current task pointer
    ├── [OP 2] stack switch (3 insns)  → permanent kernel stack
    ├── [OP 3] frame sentinel (4 insns)→ unwind chain terminates cleanly
    ├── [OP 4] scs_load_current        → shadow call stack (SCS)
    └── [OP 5] per-CPU offset (4 insns)→ tpidr_el1 = per-CPU base
```

---

## Operation 1 — `msr sp_el0, x4` (1 instruction)

**The trick:** `sp_el0` is the EL0 stack pointer. At EL1, it has no architectural
meaning as a stack register. Linux repurposes it as a "current task" fast path.

```
Before: sp_el0 = undefined / firmware garbage
After:  sp_el0 = &init_task
```

**C macro that uses it:**
```c
// arch/arm64/include/asm/current.h
static __always_inline struct task_struct *get_current(void) {
    unsigned long sp;
    asm ("mrs %0, sp_el0" : "=r"(sp));
    return (struct task_struct *)sp;
}
#define current get_current()
```

**Performance:** One `mrs` instruction = ~1 CPU cycle. Compare to x86 Linux which
uses a GS-segment relative read (`mov %gs:OFFSET, %rax`) — also ~1 cycle but requires
a memory access. ARM64's approach uses a pure register read with zero memory traffic.

---

## Operation 2 — Stack Switch (3 instructions)

```asm
ldr   x5, [x4, #TSK_STACK]    // x5 = init_task.stack = &init_stack[0]
add   sp, x5, #THREAD_SIZE    // sp = &init_stack[0] + 16384  (top of stack)
sub   sp, sp, #PT_REGS_SIZE   // sp = top - 336  (reserve pt_regs space)
```

**Key constants (from asm-offsets.c):**
```c
TSK_STACK      = offsetof(struct task_struct, stack)          // ~24 bytes
THREAD_SIZE    = 1 << THREAD_SHIFT = 1 << 14 = 16384        // 16KB on ARM64
PT_REGS_SIZE   = sizeof(struct pt_regs) = 336                // 18 x 8 = 144? No...
```

**Actual PT_REGS_SIZE breakdown:**
```c
struct pt_regs {
    union {
        struct user_pt_regs user_regs;  // x0-x30, sp, pc, pstate = 34 * 8 = 272 bytes
        struct {
            u64 regs[31];  // x0-x30
            u64 sp;
            u64 pc;
            u64 pstate;
        };
    };
    u64 orig_x0;    // 8 bytes
    s32 syscallno;  // 4 bytes
    u32 unused2;    // 4 bytes
    u64 sdei_ttbr1; // 8 bytes
    u64 pmr_save;   // 8 bytes
    u64 stackframe[2]; // 16 bytes (fp + lr) = stackframe
    u64 lockdep_hardirqs; // 8 bytes
    u64 exit_rcu;   // 8 bytes
};
// Total: 272 + 8 + 8 + 8 + 8 + 16 + 8 + 8 = 336 bytes
```

---

## Operation 3 — Frame Sentinel (4 instructions)

```asm
stp   xzr, xzr, [sp, #S_STACKFRAME]      // stackframe.fp = 0, stackframe.lr = 0
mov   x5, #FRAME_META_TYPE_FINAL          // x5 = 0x1 (the FINAL type constant)
str   x5, [sp, #S_STACKFRAME_TYPE]        // stackframe_type field = FINAL
add   x29, sp, #S_STACKFRAME              // x29 = &pt_regs.stackframe
```

**Offset calculation:**
```
S_STACKFRAME      = offsetof(struct pt_regs, stackframe)      = 288 bytes from base
S_STACKFRAME_TYPE = offsetof(struct pt_regs, stackframe) + 16 = 304 bytes from base
```

**The chain:**
```
x29 → pt_regs.stackframe { fp=0, lr=0, type=FINAL }
         │
         └─ fp=0 → unwinder STOPS HERE
```

**FRAME_META_TYPE_FINAL value:**
```c
// include/asm/stacktrace/frame.h
#define FRAME_META_TYPE_FINAL  0x1UL
```
The unwinder in `arch/arm64/kernel/stacktrace.c` checks this type field and
terminates the unwind without reading the fp field.

---

## Operation 4 — `scs_load_current` (0–2 instructions)

**Disabled build (no CONFIG_SHADOW_CALL_STACK):**
```asm
// Macro expands to nothing — zero instructions
```

**Enabled build:**
```asm
mrs   x18, sp_el0                        // x18 = &init_task (just set above)
ldr   x18, [x18, #TSK_TI_SCS_SP]        // x18 = init_task.thread_info.scs_sp
```

**TSK_TI_SCS_SP:**
```c
TSK_TI_SCS_SP = offsetof(struct task_struct, thread_info.scs_sp)
```

`thread_info.scs_sp` is the per-task shadow call stack pointer. For `init_task`,
this is initialized to point to the base of the `init_task` shadow call stack
(a separate memory region set up at compile time).

---

## Operation 5 — Per-CPU Offset (4 instructions)

```asm
adr_l   x5, __per_cpu_offset           // x5 = VA of __per_cpu_offset[0]
ldr     w6, [x4, #TSK_TI_CPU]          // w6 = init_task.thread_info.cpu = 0
ldr     x5, [x5, x6, lsl #3]           // x5 = __per_cpu_offset[0] (u64 array)
set_this_cpu_offset x5                 // msr tpidr_el1, x5
```

**`lsl #3` = multiply by 8:** `__per_cpu_offset` is a `u64` array, so index
arithmetic needs 8-byte stride: `&__per_cpu_offset[cpu] = base + cpu * 8`.

**`set_this_cpu_offset` expansion:**
```asm
// Without VHE:
msr   tpidr_el1, x5

// With VHE (HCR_EL2.E2H=1), kernel runs at EL2:
msr   tpidr_el2, x5
```

**Why `TSK_TI_CPU = 0` for init_task:**
```c
// kernel/init_task.c
struct task_struct init_task = {
    .thread_info = {
        .cpu = 0,   // boot CPU is always CPU 0
    },
    ...
};
```

**Result:** `tpidr_el1 = __per_cpu_offset[0]` = base of CPU0's per-CPU data section.
From this point, `this_cpu_read(runqueues)` works correctly on the boot CPU.

---

## Resource Acquisition Summary

```
Resource           Before init_cpu_task    After init_cpu_task
─────────────────────────────────────────────────────────────────
current task        INVALID (garbage)       init_task (PID 0)
kernel stack SP     early_init_stack        init_stack (16KB)
frame pointer x29   0                       &init_task.stack_top.stackframe
unwind chain        BROKEN                  terminates at fp=0
SCS pointer x18     undefined               init_task.thread_info.scs_sp
per-CPU base        undefined               __per_cpu_offset[0]
tpidr_el1           undefined               __per_cpu_offset[0]
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