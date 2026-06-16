# `set_task_stack_end_magic()` — Stack Overflow Sentinel

## Purpose

Writes a magic canary value (`STACK_END_MAGIC = 0x57AC6E9D`) at the bottom (lowest address) of `init_task`'s kernel stack. If this value is overwritten, the kernel knows a stack overflow has occurred and can report it rather than silently corrupting memory.

## Source File

`kernel/fork.c`

```c
void set_task_stack_end_magic(struct task_struct *tsk)
{
    unsigned long *stackend;
    stackend = end_of_stack(tsk);
    *stackend = STACK_END_MAGIC; /* for overflow detection */
}
```

## Background: Kernel Stack Layout

Each kernel task has a kernel stack of size `THREAD_SIZE` (typically 16 KB on x86-64). The stack grows downward:

```
High address: [task_struct or thread_info]
              [... stack in use ...]
              [... free stack space ...]
Low address:  [STACK_END_MAGIC]   ← set_task_stack_end_magic() writes here
```

`end_of_stack(tsk)` returns `(unsigned long *)(task_thread_info(tsk) + 1)` — the first word above `thread_info` at the bottom of the stack.

## Pre-conditions

- `init_task` must be statically allocated (it is — see `init/init_task.c`)
- Stack must be within a writable memory region

## Post-conditions

- `*end_of_stack(&init_task) == STACK_END_MAGIC`
- Stack overflow detection is armed for the boot task

## IRQ State

Interrupts may be on or off — this function does not care; it is a simple memory write.

## Key Data Structures

| Structure | Field | Purpose |
|-----------|-------|---------|
| `struct task_struct` | (via `end_of_stack()`) | Stack base pointer derivation |
| `thread_info` | embedded at stack base | `end_of_stack()` uses this |

## Call Graph

```
set_task_stack_end_magic(tsk)
    └── end_of_stack(tsk)
            └── task_thread_info(tsk)   // Arch-specific; on x86-64 returns stack bottom
```

## How Overflow is Detected Later

`check_stack_object()`, `__stack_chk_fail()`, and the stack canary mechanism all check `STACK_END_MAGIC`. Additionally, `kstack_end()` uses this to determine valid stack boundaries for stack traces.

## Kconfig Dependencies

- `CONFIG_STACK_GROWSUP`: On architectures where the stack grows upward, `end_of_stack()` is defined differently
- `CONFIG_THREAD_INFO_IN_TASK`: Controls whether `thread_info` is embedded in `task_struct` or at the stack base

## Cross-references

- [Phase overview](../README.md)
- `boot_init_stack_canary()` — per-CPU SSP canary, set much later: [../../11_security_randomness/boot_init_stack_canary/README.md](../../11_security_randomness/boot_init_stack_canary/README.md)
