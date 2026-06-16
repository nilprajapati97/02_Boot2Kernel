# `set_task_stack_end_magic()` — Stack Overflow Detection

## Overview

| Attribute    | Value                                         |
|-------------|------------------------------------------------|
| **Function** | `set_task_stack_end_magic(struct task_struct *tsk)` |
| **Source**   | `kernel/fork.c`                               |
| **Called**   | First line of `start_kernel()` — `set_task_stack_end_magic(&init_task)` |
| **Purpose**  | Place a magic canary value at the bottom of `init_task`'s kernel stack to detect stack overflow |

---

## Why It Exists

The kernel stack is a **fixed-size buffer** (typically 8KB or 16KB per task, controlled by `THREAD_SIZE`). Every function call consumes stack space via:
- Return addresses
- Saved registers
- Local variables
- `struct pt_regs` (saved at interrupt/exception entry)

If the stack overflows (grows past its allocation), it silently corrupts adjacent memory — typically the `thread_info` structure or even the `task_struct` itself (on older kernels where `thread_info` lived at the stack base). The result is **random, hard-to-debug crashes**.

The solution: place a known magic value (`STACK_END_MAGIC = 0x57AC6E9D`) at the very bottom (lowest address) of the stack allocation. A separate check function (`task_stack_end_corrupted()`) verifies this value remains intact. If it's overwritten, we know a stack overflow occurred.

---

## Prerequisites

Only `init_task` must be statically initialized — no dynamic allocation needed.

---

## What Gets Initialized

```
init_task.stack  ──►  [High addr] .... kernel stack grows down .... [Low addr]
                                                                          │
                                                            STACK_END_MAGIC written here
                                                            (at stack bottom = lowest address)
```

The magic value is placed at `end_of_stack(tsk)` which returns the lowest address of the stack:

```c
static inline unsigned long *end_of_stack(struct task_struct *t)
{
#ifdef CONFIG_KASAN_STACK
    return t->stack;
#else
    return (unsigned long *)(task_thread_info(t) + 1);
#endif
}
```

---

## Internal Deep Dive

### Source Code Walkthrough

```c
// kernel/fork.c
void set_task_stack_end_magic(struct task_struct *tsk)
{
    unsigned long *stackend;

    stackend = end_of_stack(tsk);      // ptr to lowest stack address
    *stackend = STACK_END_MAGIC;       // write 0x57AC6E9D
}
```

Simple — but critical. The value `0x57AC6E9D` was chosen because:
- It is unlikely to appear naturally in stack data
- It is recognizable in hex dump as "STACK END MAGIC"
- 0x57 = 'W', 0xAC, 0x6E, 0x9D → mnemonic in kernel dumps

### Verification at Runtime

The `task_stack_end_corrupted()` function is called periodically:
```c
// include/linux/sched/task_stack.h
static inline int task_stack_end_corrupted(struct task_struct *p)
{
    return unlikely(*end_of_stack(p) != STACK_END_MAGIC);
}
```

This is called in:
- `exit_to_user_mode_prepare()` — before returning to userspace
- `do_task_stat()` — when `/proc/<pid>/stat` is read
- `kstack_end()` — during stack unwinding (ftrace, lockdep)

---

## Key Data Structures

### `task_struct` (relevant fields)
```c
struct task_struct {
    void                *stack;         // pointer to kernel stack allocation
    // ...
};
```

### `thread_info` (at stack base on most architectures)
```c
struct thread_info {
    unsigned long   flags;              // TIF_* flags
    int             preempt_count;      // preemption/bh count
    mm_segment_t    addr_limit;         // user/kernel address boundary
    // ...
};
```

On x86, `thread_info` is at the **bottom** of the stack allocation. `STACK_END_MAGIC` goes just **below** `thread_info` (at `thread_info - sizeof(unsigned long)`).

---

## Stack Layout Diagram

```
Higher address (stack top, initial RSP)
┌──────────────────────────────────┐  ← init_task.stack + THREAD_SIZE
│                                  │
│    Kernel stack (grows downward) │
│    ↓  ↓  ↓                       │
│                                  │
│    ... (empty stack space) ...   │
│                                  │
│    ↓  ↓  ↓                       │
│    [thread_info struct]          │  ← task_thread_info(init_task)
│    [STACK_END_MAGIC: 0x57AC6E9D] │  ← end_of_stack(init_task)
└──────────────────────────────────┘  ← init_task.stack (base address)
Lower address (stack bottom)
```

---

## Interaction With Other Subsystems

| Subsystem | Interaction |
|-----------|-------------|
| **fork()** | Every `copy_process()` call also sets the magic on the new task's stack |
| **KASAN** | KASAN shadow maps track accesses to the stack; magic complements this |
| **Stack unwinding** | `stack_trace_save()` uses `kstack_end()` which checks the magic |
| **Watchdog** | Kernel soft/hardlockup detector checks for stack corruption |
| **/proc** | `task_stack_end_corrupted()` exposed via `/proc/<pid>/stack` debugging |

---

## Sub-Topics (Deep Dive)

- [01_init_task_and_task_struct](01_init_task_and_task_struct/README.md) — What is `init_task`? How is it statically initialized?
- [02_stack_overflow_detection](02_stack_overflow_detection/README.md) — How does the kernel detect and handle stack overflow at runtime?

---

## Interview Q&A — NVIDIA / Google / Qualcomm Level

### Q1: Why is `set_task_stack_end_magic()` the very first call in `start_kernel()`?
**A:** Because it protects the boot CPU's stack from overflow during the extremely deep call stacks that follow in `start_kernel()`. If setup_arch or mm_core_init causes a stack overflow, we need the detection mechanism armed. Setting it last would defeat its purpose — it must be set before any deep code runs.

### Q2: What is `STACK_END_MAGIC` and what happens if it's overwritten?
**A:** `STACK_END_MAGIC = 0x57AC6E9D`. It is placed at `end_of_stack(task)`. If a stack overflow occurs, this value gets overwritten by function arguments or return addresses. `task_stack_end_corrupted()` returns `true`, and the kernel's periodic checks (syscall exit path, `/proc` reads) will detect this and trigger a `BUG()` or `KERN_EMERG` message. Without this, the overflow silently corrupts the `thread_info` flags, leading to bizarre scheduling or security bugs.

### Q3: How does the stack layout differ between old kernels (pre-4.9) and newer kernels?
**A:** Before kernel 4.9, `thread_info` was embedded at the **base of the kernel stack** — stack overflow directly corrupted scheduling flags, security flags, and preemption count. From kernel 4.9 onward, `thread_info` was moved into `task_struct` itself (off-stack) on x86, so stack overflow no longer silently corrupts these critical fields. However, the magic canary is still needed to detect overflow before it corrupts other stack frames.

### Q4: How many bytes does `STACK_END_MAGIC` occupy and where exactly?
**A:** It occupies `sizeof(unsigned long)` bytes — 8 bytes on 64-bit, 4 bytes on 32-bit. It is written to the address returned by `end_of_stack(tsk)`, which is just above the lowest address of the stack allocation (accounting for any guard pages). On x86-64, this is at `init_stack` base address.

### Q5: What is the difference between the stack end magic and KASAN stack poisoning?
**A:** Stack end magic is a single sentinel value at the **very bottom** of the stack — coarse-grained detection. KASAN (Kernel Address Sanitizer) maintains a shadow map of the entire stack and detects accesses to **any uninitialized or out-of-bounds** stack memory. KASAN catches more bugs (use-after-return, out-of-bounds within a frame) but has significant memory and performance overhead. Stack end magic has near-zero overhead and is always enabled.

### Q6: How does `fork()` ensure new tasks also get stack overflow protection?
**A:** In `copy_process()` → `dup_task_struct()` → `setup_thread_stack()`, after the new kernel stack is allocated and the parent's `thread_info` is copied, `setup_thread_stack()` calls `clear_tsk_thread_flag()` and importantly, `set_task_stack_end_magic(tsk)` is called on the new task. This ensures every task gets its own magic canary at its stack base. This is in `kernel/fork.c`.

### Q7: Can the stack end magic be bypassed or cause false positives?
**A:** False positives are theoretically possible if a legitimate stack value happens to be `0x57AC6E9D`, but the probability is negligible (1 in 2^32 per 8-byte write). A stack that has grown exactly to its limit and written this value would be an extraordinary coincidence. False negatives are possible — if the stack overflows by more than `THREAD_SIZE`, the magic value location itself is overwritten AND the memory below it, but the check would still trigger since the magic is gone.

---

## Common Bugs and Pitfalls

| Issue | Description |
|-------|-------------|
| **Per-CPU stack overflow** | On x86, interrupt handlers use separate interrupt stacks (IST) — `STACK_END_MAGIC` only protects the task kernel stack, not IST stacks |
| **Guard pages** | Some configurations add virtual guard pages that trigger page faults before the magic is hit — magic is a backup to guard pages |
| **64KB stacks with KASAN** | KASAN's shadow memory doubles effective stack size, so THREAD_SIZE must be increased when KASAN is enabled |
| **Unwinder false detection** | Frame-pointer-based unwinders stop at `kstack_end()` which checks for magic — a corrupted magic stops unwinding prematurely |
