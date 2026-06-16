# ARM64 Stack Unwinder Behavior — Deep Dive

## Entry Points to Stack Unwinding

The stack unwinder is invoked from multiple kernel subsystems:

```c
// 1. Oops/Panic handler:
show_stack(current, NULL, KERN_DEFAULT);

// 2. WARN/BUG macros:
WARN_ON(cond);  // internally calls dump_stack()

// 3. Dynamic ftrace (function call trace):
ftrace_caller → record_fp → stacktrace_save

// 4. Perf subsystem:
perf_callchain_kernel → arch_perf_callchain_kernel

// 5. kprobes:
kprobe_handler → show_backtrace

// 6. /proc/PID/stack (task stack dump)
proc_pid_stack → stack_trace_save
```

All of these eventually reach `arch_stack_walk` or `unwind_frame`:
```c
// arch/arm64/kernel/stacktrace.c
void arch_stack_walk(stack_trace_consume_fn consume_entry, void *cookie,
                     struct task_struct *task, struct pt_regs *regs)
```

---

## Full Unwinder Algorithm

```c
struct unwind_state {
    unsigned long fp;       // current frame pointer (x29)
    unsigned long pc;       // current PC / return address
    struct task_struct *task;
    struct pt_regs *regs;   // if crossing exception boundary
};

void arch_stack_walk(...)
{
    struct unwind_state state;

    // Initialize: start from current fp/pc
    state.fp = (unsigned long)__builtin_frame_address(0);  // x29
    state.pc = (unsigned long)arch_stack_walk;              // current PC

    while (1) {
        // Consume current frame (call user's callback)
        if (!consume_entry(cookie, state.pc, false))
            break;

        // Advance to next frame
        int ret = unwind_next_frame(&state);
        if (ret < 0)
            break;   // -ENOENT = normal termination; -EINVAL = corruption
    }
}
```

---

## `unwind_next_frame` — The Core Step

```c
static int notrace unwind_next_frame(struct unwind_state *state)
{
    unsigned long fp = state->fp;

    // [Check 1] fp must be 16-byte aligned (AAPCS64 requirement)
    if (fp & 0xf) {
        pr_warn("fp misaligned: %lx\n", fp);
        return -EINVAL;
    }

    // [Check 2] fp must be on a kernel stack (not user memory, not I/O memory)
    if (!on_accessible_stack(state->task, fp, sizeof(struct frame_record), NULL)) {
        pr_warn("fp not on valid stack: %lx\n", fp);
        return -EINVAL;
    }

    struct frame_record *record = (struct frame_record *)fp;

    // [Check 3] Type-based dispatch
    if (record->type == FRAME_META_TYPE_FINAL) {
        // Normal bottom-of-stack termination
        return -ENOENT;
    }

    if (record->type == FRAME_META_TYPE_PT_REGS) {
        // Exception boundary — recover pt_regs context
        struct pt_regs *regs = container_of(record, struct pt_regs, stackframe);
        state->fp = regs->regs[29];  // fp at time of exception
        state->pc = regs->pc;        // pc at time of exception
        state->regs = regs;          // save for reference
        return 0;
    }

    // [Default] Follow fp chain
    if (record->fp == 0) {
        // Legacy check for old-style termination
        return -ENOENT;
    }

    // Advance state
    state->pc = record->pc;    // return address = caller's LR
    state->fp = record->fp;    // caller's frame pointer
    return 0;
}
```

---

## Stack Validity Checks — Security Requirement

The unwinder validates that `fp` stays within known kernel stacks. This prevents:

1. **Information disclosure:** A buggy or malicious `fp` value could cause the
   unwinder to read arbitrary kernel memory and print it to logs (visible to user).

2. **Arbitrary code execution:** A crafted `fp` chain could cause the unwinder
   to execute a callback with attacker-controlled PC values.

The check `on_accessible_stack` verifies:
```c
// fp must be within one of:
// 1. The current task's kernel stack (init_stack for init_task)
// 2. A per-CPU IRQ stack
// 3. A per-CPU NMI/overflow stack
// 4. A mapped vmalloc stack (for other tasks with VMAP_STACK)
```

---

## What the Boot Frame Sentinel Looks Like to the Unwinder

When the unwinder reaches the boot frame:

```
state.fp = boot_frame_sentinel_VA = sp + S_STACKFRAME
                                  = init_stack_base + 16384 - 336 + 304
                                  = init_stack_base + 16352

record = (struct frame_record *)(init_stack_base + 16352)
record->fp   = 0
record->pc   = 0
record->type = 1 (FRAME_META_TYPE_FINAL)
```

Check 1 (alignment): 16352 % 16 = 0 ✓ (16352 = 1022 × 16)
Check 2 (valid stack): yes, within init_stack ✓
Check 3 (type): FRAME_META_TYPE_FINAL → return -ENOENT

Unwinder terminates cleanly. The call trace output ends at the frame just before
the boot frame (typically `start_kernel` or `kernel_init`).

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the stack pointer is a dedicated register (SP_EL1 at EL1, SP_EL0 at EL0). SP_EL1 is the stack pointer used by the kernel during normal execution. The AAPCS64 ABI requires the stack to be 16-byte aligned at any instruction that may cause an exception. SCTLR_EL1.SA (bit 3) enables hardware enforcement of this alignment: if SP_EL1 is not 16-byte aligned when a load/store using SP is executed, an SP alignment fault is raised. The frame pointer (x29) is a general-purpose register used by convention to hold the base of the current stack frame. Writing x29 is the first act of any C function that wishes to be unwound.

### Kernel Perspective (Linux ARM64)
After the MMU is enabled, __primary_switch reinitializes the stack pointer to a virtual address. The early boot stack is defined as:
  __INIT_DATA: init_thread_union (size THREAD_SIZE, typically 16 KB)
The LDR instruction loads the VA of init_thread_union + THREAD_SIZE into x0, then MOV sp, x0 sets SP_EL1. This is necessary because the old stack pointer was set to a physical address (before the MMU) and that PA is no longer the correct address for the kernel VA layout. x29 is set to zero (zero frame pointer) to terminate the unwind chain at the first kernel stack frame.

### Memory Perspective (ARMv8 Memory Model)
The kernel stack resides in Normal Inner-Shareable Write-Back Cacheable memory (MT_NORMAL). Once the MMU and D-cache are enabled, all stack accesses (PUSH/POP equivalents: STP/LDP) go through the L1 D-cache. The L1 D-cache write-back policy means that the stack contents are not immediately visible to physical memory until a cache clean or eviction. This is safe for the stack because the kernel does not use DMA to read stack memory. The stack pointer reinitalization at VA is a hard cut: all future kernel stack frames exist in the high VA kernel mapping.