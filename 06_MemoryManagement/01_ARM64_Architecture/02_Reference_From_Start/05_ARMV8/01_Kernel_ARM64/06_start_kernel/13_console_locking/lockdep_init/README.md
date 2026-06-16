# `lockdep_init()` — Lock Dependency Tracking

## Purpose

Initializes lockdep, the kernel's lock dependency graph validator. Lockdep detects potential deadlocks by tracking the order in which locks are acquired, building a directed graph, and reporting any cycles that represent impossible lock orderings.

## Source File

`kernel/locking/lockdep.c`

```c
void lockdep_init(void)
{
    int i;
    
    if (lockdep_initialized)
        return;
    
    // Initialize hash tables for:
    for (i = 0; i < CLASSHASH_SIZE; i++)
        INIT_LIST_HEAD(classhash_table + i);  // Lock classes
    
    for (i = 0; i < CHAINHASH_SIZE; i++)
        INIT_LIST_HEAD(chainhash_table + i);  // Lock chains
    
    lockdep_initialized = 1;
}
```

## Core Concepts

### Lock Classes

Every lock is associated with a "class" — the static definition of the lock (not the instance):

```c
// Two instances of the same type → same class:
spinlock_t lock1;  // class: "lock1" at file.c:42
spinlock_t lock2;  // class: "lock2" at file.c:43

// But these share a class because they're the same named lock:
DEFINE_SPINLOCK(my_lock);  // class: "my_lock"
```

### Dependency Graph

```
If CPU 0 does:    lock(A); lock(B); unlock(B); unlock(A);
    → adds edge A → B (A must be acquired before B)

If CPU 1 does:    lock(B); lock(A); unlock(A); unlock(B);
    → adds edge B → A

Result: A → B → A  (CYCLE = potential deadlock)
lockdep reports: WARNING: possible circular locking
```

### Lock Chain Hash

Lockdep tracks the entire sequence of held locks (a "chain") to detect multi-lock ordering violations:

```
Chain: [A, B, C] means: while holding A and B, trying to acquire C
```

## What Lockdep Detects

1. **Deadlock cycles**: Lock A → B → A
2. **HARDIRQ-safe/unsafe conflicts**: A regular lock acquired in IRQ context that could deadlock with a lock held while IRQs are disabled
3. **SOFTIRQ-safe/unsafe conflicts**: Similar for softirq context
4. **Recursive locking**: Non-recursive locks acquired twice
5. **Lock ordering inconsistency**: Different files lock in different order

## Example Report

```
======================================================
WARNING: possible circular locking dependency detected
------------------------------------------------------
task/1234 is trying to acquire lock:
   (&inode->i_lock){+.+.}-{3:3}

but task is already holding:
   (&sb->s_umount){++++}-{3:3}

which lock already depends on the new lock.

the existing dependency chain (in reverse order) is:

-> #1 (&sb->s_umount){++++}-{3:3}:
       lock_acquire+0x...
       down_read+0x...
       ...

-> #0 (&inode->i_lock){+.+.}-{3:3}:
       lock_acquire+0x...
       spin_lock+0x...
       ...
======================================================
```

## Enabling Lockdep

Lockdep requires kernel config options:

```
CONFIG_PROVE_LOCKING=y      # Core lockdep
CONFIG_LOCK_STAT=y          # Lock contention statistics
CONFIG_DEBUG_LOCKDEP=y      # Extra lockdep assertions
CONFIG_LOCKDEP=y            # Implied by PROVE_LOCKING
```

## Performance Cost

Lockdep adds overhead to every lock/unlock operation:
- Normal kernel (no lockdep): ~1ns per lock/unlock
- Lockdep enabled: ~50-200ns per lock/unlock

It is always disabled in production kernels and enabled only for kernel development/testing.

## Cross-references

- [Phase overview](../README.md)
- `locking_selftest()`: [../locking_selftest/README.md](../locking_selftest/README.md)
