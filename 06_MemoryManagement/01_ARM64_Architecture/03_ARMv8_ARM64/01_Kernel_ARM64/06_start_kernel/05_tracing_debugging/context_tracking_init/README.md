# `context_tracking_init()` — User/Kernel Context Tracking

## Purpose

Initializes the context tracking subsystem, which tracks transitions between user space and kernel space on a per-CPU basis. This information is used by RCU (for extended quiescent states), NO_HZ_FULL (tickless operation), and virtual time accounting.

## Source File

`kernel/context_tracking.c`

```c
void __init context_tracking_init(void)
{
    int cpu;
    
    for_each_possible_cpu(cpu)
        atomic_set(&per_cpu(context_tracking.state, cpu), 
                   CONTEXT_KERNEL);
}
```

## The Context Tracking States

```c
enum ctx_state {
    CONTEXT_DISABLED = -1,  // Context tracking disabled on this CPU
    CONTEXT_KERNEL   =  0,  // Currently in kernel space
    CONTEXT_IDLE     =  1,  // CPU is idle
    CONTEXT_USER     =  2,  // Currently in user space
    CONTEXT_GUEST    =  3,  // Currently in guest (VM) context
    CONTEXT_MAX      =  4,
};
```

## Why Track Contexts?

### 1. RCU Extended Quiescent States

RCU (Read-Copy-Update) needs to know when CPUs are in "quiescent states" (not holding RCU read locks). A CPU in user space is always in a quiescent state — RCU read-side critical sections cannot span user-space execution.

Without context tracking: RCU requires the scheduler tick to detect quiescent states.
With context tracking: User↔kernel transitions directly signal quiescent states — enabling NO_HZ_FULL (the scheduler tick can be stopped for CPUs running user-only tasks).

### 2. NO_HZ_FULL (Full Tickless Operation)

```
Without context_tracking:
    tick fires every 1ms even in user space → wakes CPU, causes latency

With context_tracking + NO_HZ_FULL:
    CPU enters user space → tick stopped
    CPU returns to kernel → tick restarted
```

This is critical for HPC (High Performance Computing) workloads and real-time applications.

### 3. Virtual Time Accounting

On paravirtual machines, tracking user/kernel time requires knowing which context the CPU is in.

## The Entry/Exit Hooks

Context tracking inserts hooks at all user↔kernel transitions:

```c
// Called when transitioning from kernel to user:
void user_enter(void)
{
    if (context_tracking_is_enabled())
        __context_tracking_enter(CONTEXT_USER);
}

// Called when returning to kernel:
void user_exit(void)
{
    if (context_tracking_is_enabled())
        __context_tracking_exit(CONTEXT_USER);
}
```

These hooks are placed in:
- `entry_64.S` (syscall entry/exit)
- `entry_64_compat.S` (32-bit syscall entry)
- Exception handlers

## Overhead

When `CONFIG_CONTEXT_TRACKING` is enabled but no subscribers (like NO_HZ_FULL):
- The tracking calls are NOPs (via static keys)

When subscribers are active:
- ~10–50ns per user↔kernel transition

## Kconfig Dependencies

- `CONFIG_CONTEXT_TRACKING`: Base infrastructure
- `CONFIG_CONTEXT_TRACKING_IDLE`: Track CPU idle state
- `CONFIG_NO_HZ_FULL`: Full tickless — requires CONTEXT_TRACKING

## Cross-references

- [Phase overview](../README.md)
- RCU: [../../08_rcu/rcu_init/README.md](../../08_rcu/rcu_init/README.md)
