# `pid_idr_init()` — PID Namespace Initialization

## Purpose

Initializes the PID (Process ID) allocation infrastructure. Sets up the IDR (ID Radix) data structure used to allocate and look up PIDs within the root PID namespace.

## Source File

`kernel/pid.c`

```c
void __init pid_idr_init(void)
{
    // Allocate the PID slab cache:
    pid_cachep = KMEM_CACHE(pid,
                            SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT);
    
    // Reserve PID 0 and PID 1:
    // PID 0 = swapper (idle thread)
    // PID 1 = init process (to be created by rest_init)
    idr_preload(GFP_KERNEL);
    spin_lock_irq(&pidmap_lock);
    
    // Allocate PID 1 to ensure init gets it:
    idr_alloc(&init_pid_ns.idr, NULL, 1, 2, GFP_ATOMIC);
    
    spin_unlock_irq(&pidmap_lock);
    idr_preload_end();
    
    init_pid_ns.pid_cachep = pid_cachep;
}
```

## PID Namespaces

Linux supports PID namespaces (for containers). Each namespace has its own PID counter starting at 1:

```
Root namespace:
  PID 1  = /sbin/init
  PID 2  = kthreadd
  PID 100 = sshd
  PID 1000 = container process A  ←──┐
                                      │ same process
Container namespace:                  │
  PID 1  = container init      ←──┘  
  PID 5  = bash in container
```

## `struct pid`

```c
struct pid {
    refcount_t      count;
    unsigned int    level;          // Depth in namespace tree
    spinlock_t      lock;
    struct hlist_head tasks[PIDTYPE_MAX]; // Processes using this PID
    struct hlist_head inodes;
    wait_queue_head_t wait_pidfd;
    struct rcu_head rcu;
    struct upid      numbers[];    // One entry per namespace level
};

// PID types:
enum pid_type {
    PIDTYPE_PID,    // process PID (getpid())
    PIDTYPE_TGID,   // thread group ID (tgid in task_struct)
    PIDTYPE_PGID,   // process group ID (getpgrp())
    PIDTYPE_SID,    // session ID (getsid())
    PIDTYPE_MAX
};
```

## IDR — ID Radix Tree

The IDR (ID Radix) is a radix tree mapping integer IDs to pointers:

```c
// Allocate a new PID:
pid = idr_alloc(&ns->idr, pid_struct, min, max, GFP_KERNEL);

// Look up a PID:
pid = idr_find(&ns->idr, pid_number);

// Remove a PID:
idr_remove(&ns->idr, pid_number);
```

IDR replaced the old bitmap-based pidmap for better scalability.

## PID_MAX

The default maximum PID is 32768 (32-bit) or 4194304 (64-bit):

```bash
cat /proc/sys/kernel/pid_max    # Default: 32768
echo 4194304 > /proc/sys/kernel/pid_max  # Increase for large systems
```

## Cross-references

- [Phase overview](../README.md)
- `fork_init()`: [../fork_init/README.md](../fork_init/README.md) — uses PID allocation
