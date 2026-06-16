# `rest_init()` — Kernel Thread Spawning and Idle

## Purpose

Spawns the first two kernel threads (PID 1 and PID 2), enables RCU scheduling, and transforms the boot CPU (CPU 0) into the idle thread. This is the last function ever called on the boot path — `rest_init()` never returns.

## Source File

`init/main.c`

## Why "rest_init"?

The name means "initialize the rest of the system." By this point, the basic kernel infrastructure is ready, but:
- No user processes exist yet
- Only one CPU is online
- The kernel is still running as the `init_task` (the "swapper" process, PID 0)

`rest_init()` creates the threads that will complete initialization and eventually run userspace.

## `rcu_scheduler_starting()`

See [rcu_scheduler_starting/README.md](rcu_scheduler_starting/README.md). Before this call, RCU used a simplified mode that assumed no preemption. After this call, RCU properly accounts for preempted readers.

## `kernel_thread(kernel_init, NULL, CLONE_FS)`

Creates a new kernel thread that will run `kernel_init()`. This thread:
- Gets PID 1 (reserved by `pid_idr_init()`)
- Starts as a kernel thread (no user address space yet)
- Will eventually `execve()` `/sbin/init` to become the init process

The `CLONE_FS` flag means it shares the filesystem namespace with the boot thread initially.

## `kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES)`

Creates the kthreadd daemon:
- Gets PID 2
- Runs `kthreadd()` which processes requests to spawn kernel threads
- All future `kthread_create()` calls go through kthreadd

## `complete(&kthreadd_done)`

After kthreadd is created, `complete()` releases any threads waiting for kthreadd to be ready. The `kernel_init` thread waits on `kthreadd_done` before proceeding.

## CPU 0 → Idle Thread

```c
init_idle_bootup_task(current);  // Configure init_task as idle
schedule_preempt_disabled();      // Switch to a real task (PID 1 or 2)
cpu_startup_entry(CPUHP_ONLINE);  // Enter idle loop
```

After `schedule_preempt_disabled()`, the scheduler runs and switches to PID 1 or PID 2. CPU 0's `init_task` (PID 0 / swapper) becomes the idle task and runs `do_idle()` whenever no other task is runnable on CPU 0.

## The Three Processes

```
PID 0 (swapper/0):
    - Created statically at compile time as init_task
    - Becomes the idle task for CPU 0
    - Runs cpu_startup_entry() → do_idle() → HLT instruction

PID 1 (kernel_init → /sbin/init):
    - Created by rest_init() via kernel_thread()
    - Completes kernel_init_freeable() setup
    - exec()s /sbin/init, /init, /bin/sh (trying in order)
    - The ancestor of all user processes

PID 2 (kthreadd):
    - Created by rest_init() via kernel_thread()
    - Runs kthreadd() loop forever
    - Parent of all kernel threads (kworker, ksoftirqd, etc.)
```

## Cross-references

- [Phase overview](../README.md)
- [`kernel_init/`](../kernel_init/README.md) — PID 1 detailed path
- [`kthreadd/`](../kthreadd/README.md) — PID 2 kernel daemon
