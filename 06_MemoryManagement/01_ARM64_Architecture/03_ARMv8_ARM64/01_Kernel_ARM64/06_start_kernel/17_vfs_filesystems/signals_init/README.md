# `signals_init()` — Signal Queue Slab Cache

## Purpose

Creates the slab cache for `struct sigqueue`, the kernel object used to enqueue real-time signals (SIGRTMIN through SIGRTMAX) that need to carry payload data (`siginfo_t`).

## Source File

`kernel/signal.c`

```c
void __init signals_init(void)
{
    sigqueue_cachep = KMEM_CACHE(sigqueue,
                                  SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT);
}
```

## Signal Types

### Standard (Non-Real-Time) Signals

Signals 1-31 (SIGHUP, SIGINT, SIGKILL, etc.):
- Not queued — only one pending per signal type per process
- No payload data (just the signal number)
- Delivered in an unspecified order

### Real-Time Signals (POSIX)

Signals SIGRTMIN through SIGRTMAX (32-64 on Linux):
- **Queued** — multiple instances can be pending
- Carry payload data via `siginfo_t`
- Delivered in FIFO order (lowest-numbered first)
- Used by `sigqueue()`, timers, AIO, message queues

## `struct sigqueue`

```c
struct sigqueue {
    struct list_head    list;    // In task's pending signal list
    int                 flags;   // SIGQUEUE_PREALLOC flag
    kernel_siginfo_t    info;    // Signal info + payload
    struct ucounts      *ucounts; // Per-user queue limit tracking
};
```

## `siginfo_t` Payload

```c
typedef struct kernel_siginfo {
    int si_signo;    // Signal number
    int si_errno;    // Error code (if applicable)
    int si_code;     // Origin of signal (SI_USER, SI_KERNEL, SI_TIMER, ...)
    
    union {
        // Kill signal:
        struct { pid_t pid; uid_t uid; } _kill;
        
        // POSIX timer:
        struct { timer_t tid; int overrun; sigval_t sigval; } _timer;
        
        // POSIX message queue:
        struct { int band; int fd; } _sigpoll;
        
        // Fault signal (SIGSEGV, SIGBUS):
        struct { void *addr; short addr_lsb; } _sigfault;
        
        // Real-time user payload:
        struct { pid_t pid; uid_t uid; sigval_t sigval; } _rt;
    } _sifields;
} kernel_siginfo_t;
```

## Signal Pending Queue

Each process has two pending signal queues:
1. `task->pending` — thread-specific signals
2. `task->signal->shared_pending` — process-wide signals

```c
struct sigpending {
    struct list_head    list;   // List of sigqueue entries
    sigset_t            signal; // Bitmask of pending signals (for fast check)
};
```

## Per-User Queue Limit

To prevent DoS (flooding a process with real-time signals), the kernel limits signal queue depth per user:

```bash
# View/set signal queue limit:
ulimit -i           # Shows SIGQUEUE limit
ulimit -i 65536     # Increase it
```

## Cross-references

- [Phase overview](../README.md)
- `proc_caches_init()`: [../../15_process_management/proc_caches_init/README.md](../../15_process_management/proc_caches_init/README.md)
