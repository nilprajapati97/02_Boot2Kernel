# `task_struct` Memory Layout

## Kernel Stack and task_struct

Every process has a kernel stack. On x86-64, the default size is 16KB (two 4KB pages, or `THREAD_SIZE`).

## Physical Layout

```
High address (RSP starts here when entering kernel):
┌─────────────────────────────────────────────────┐  top_of_stack
│                                                   │
│         Kernel Stack                              │
│         (grows downward)                          │
│         ~16KB usable                              │
│                                                   │
│  ┌─────────────────────────────────────────────┐ │
│  │ pt_regs (user register save area)            │ │  ← when syscall/interrupt
│  │   rax, rbx, rcx, rdx, rsi, rdi, ...         │ │
│  │   rip, rsp, rflags, cs, ss                   │ │
│  └─────────────────────────────────────────────┘ │
│                                                   │
│         ... more stack frames ...                 │
│                                                   │
├─────────────────────────────────────────────────┤
│         STACK_END_MAGIC (0x57AC6E9F4E7)           │  ← overflow detector
├─────────────────────────────────────────────────┤
│         struct thread_info                        │
│           .flags (TIF_NEED_RESCHED, etc.)         │
│           .preempt_count                          │
│           .addr_limit                             │
│           .task → (back pointer to task_struct)  │
└─────────────────────────────────────────────────┘  low address
```

## `thread_info` vs `task_struct`

On modern kernels with `CONFIG_THREAD_INFO_IN_TASK=y`:

```c
// thread_info is the FIRST field of task_struct:
struct task_struct {
    struct thread_info  thread_info;  // Must be first!
    volatile long       state;
    // ...
};
```

This means the RSP can compute the `task_struct` address:
```c
// current task from stack pointer:
struct task_struct *current = (struct task_struct *)
    (current_stack_pointer & ~(THREAD_SIZE - 1));
```

## `current` Macro

On x86-64, the `current` macro uses the `%gs` segment base, which points to the per-CPU `current_task` pointer:

```c
// arch/x86/include/asm/current.h:
static __always_inline struct task_struct *get_current(void)
{
    return this_cpu_read_stable(current_task);
}
#define current get_current()
```

The per-CPU `current_task` is updated on every context switch:

```c
// In __switch_to():
this_cpu_write(current_task, next_p);
```

## Stack Overflow Protection

Three mechanisms protect against stack overflow:
1. `STACK_END_MAGIC` at bottom (detects after the fact)
2. Guard page (unmapped page below stack — hardware trap)
3. Shadow stack (Control Flow Integrity, newer CPUs)

## Cross-references

- [fork_init](../README.md) — parent document
- `set_task_stack_end_magic()`: [../../../01_early_cpu_task_setup/set_task_stack_end_magic/README.md](../../../01_early_cpu_task_setup/set_task_stack_end_magic/README.md)
