# 13 — Lockdep: Lock Dependency Debugging

> **Scope**: CONFIG_PROVE_LOCKING, lock classes, dependency graph, lockdep annotations, common lockdep warnings, and how to read/fix lockdep splats.

---

## 1. What is Lockdep?

Lockdep is the kernel's **runtime lock dependency validator**. It tracks every lock acquisition and builds a dependency graph. If it detects a potential deadlock (dependency cycle), it prints a warning — even if the deadlock hasn't actually happened yet.

```mermaid
flowchart TD
    subgraph Lockdep["Lockdep - Lock Dependency Validator"]
        L1["Tracks: which lock classes exist"]
        L2["Tracks: lock acquisition order"]
        L3["Builds: directed dependency graph"]
        L4["Detects: cycles in the graph = potential deadlock"]
        L5["Reports: BEFORE actual deadlock occurs"]
    end
    
    subgraph Enable["Enable with"]
        E1["CONFIG_PROVE_LOCKING=y"]
        E2["CONFIG_DEBUG_LOCK_ALLOC=y"]
        E3["CONFIG_LOCKDEP=y"]
    end
    
    style L4 fill:#e74c3c,stroke:#c0392b,color:#fff
    style L5 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 2. Lock Classes — Not Instances

```mermaid
flowchart TD
    subgraph Classes["Lock Class vs Lock Instance"]
        CLASS["Lock CLASS = all locks at the<br/>same code location point of init"]
        
        INST1["spinlock A in driver1_probe<br/>Instance 1"]
        INST2["spinlock A in driver1_probe<br/>Instance 2"]
        INST3["spinlock B in driver2_probe<br/>Different class"]
        
        CLASS --> INST1
        CLASS --> INST2
        CLASS -.-> INST3
    end
    
    subgraph Why["Why Classes?"]
        W1["There are millions of lock instances"]
        W2["Tracking each instance is too expensive"]
        W3["Locks created at same code location<br/>have SAME ordering rules"]
        W4["So lockdep tracks maybe 8000 classes<br/>instead of millions of instances"]
    end
    
    style CLASS fill:#3498db,stroke:#2980b9,color:#fff
    style INST3 fill:#f39c12,stroke:#e67e22,color:#fff
```

```c
/* Two locks initialized at different lines = different classes */
spin_lock_init(&dev->lock_a);  /* Class A */
spin_lock_init(&dev->lock_b);  /* Class B */

/* If CPU 0: lock A → lock B
 * And CPU 1: lock B → lock A
 * Lockdep detects the ordering inversion immediately */
```

---

## 3. Dependency Graph and Cycle Detection

```mermaid
flowchart TD
    subgraph Graph["Lock Dependency Graph"]
        LA["Lock Class A"]
        LB["Lock Class B"]
        LC["Lock Class C"]
        
        LA -->|"A acquired then B"| LB
        LB -->|"B acquired then C"| LC
        LC -->|"C acquired then A"| LA
    end
    
    subgraph Detect["Cycle Detection"]
        D1["A then B then C then A"]
        D2["This is a CYCLE"]
        D3["Potential DEADLOCK"]
        D4["Lockdep reports immediately<br/>even on first occurrence"]
    end
    
    Graph --> Detect

    style D3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style D4 fill:#f39c12,stroke:#e67e22,color:#fff
```

### Deadlock Scenario:

```mermaid
sequenceDiagram
    participant CPU0 as CPU 0
    participant CPU1 as CPU 1

    CPU0->>CPU0: lock A
    CPU1->>CPU1: lock B
    CPU0->>CPU0: lock B - BLOCKED
    CPU1->>CPU1: lock A - BLOCKED

    Note over CPU0,CPU1: DEADLOCK<br/>Lockdep would have caught this<br/>on the FIRST time lock B was<br/>taken after A on ANY CPU
```

---

## 4. Reading a Lockdep Splat

```
=============================================
[ INFO: possible circular locking dependency detected ]
5.10.0 #1 Not tainted
---------------------------------------------
process_A/1234 is trying to acquire lock:
 ffff8880a1b2c3d0 (&dev->lock_b){+.+.}, at: do_something+0x42/0x80

but task is already holding lock:
 ffff8880a1b2c3e0 (&dev->lock_a){+.+.}, at: my_function+0x23/0x60

which lock already depends on the new lock.

the existing dependency chain (in reverse order) is:

-> #1 (&dev->lock_b){+.+.}:
       lock_acquire+0xb5/0x200
       _raw_spin_lock+0x30/0x40
       other_function+0x45/0x90   ← lock B acquired here
       ...

-> #0 (&dev->lock_a){+.+.}:
       lock_acquire+0xb5/0x200
       _raw_spin_lock+0x30/0x40
       yet_another_func+0x33/0x70  ← lock A acquired while B held
       ...

other info that might help us debug this:
 Possible unsafe locking scenario:
       CPU0                    CPU1
       ----                    ----
  lock(&dev->lock_a);
                               lock(&dev->lock_b);
                               lock(&dev->lock_a);  ← DEADLOCK
  lock(&dev->lock_b);
```

### Lock State Annotations:

```
{+.+.} = { hardirq, softirq, in_hardirq, in_softirq }
 +  = lock used in this context
 -  = lock NOT used in this context
 .  = irq state irrelevant

{-.-.}  = Never used in IRQ context
{+.+.}  = Used in both hardirq and softirq
{..+.}  = Used when hardirq is running
```

---

## 5. Lockdep Annotations

```c
/* Nest lock — same class taken twice with subclass */
spin_lock_nested(&child->lock, SINGLE_DEPTH_NESTING);
/* Tells lockdep: this is a DIFFERENT level, not a bug */

/* Lock class key for dynamic locks */
static struct lock_class_key my_lock_key;
lockdep_set_class(&my_lock, &my_lock_key);
/* Assigns a custom class to a lock */

/* Subclass for ordered lists of same-class locks */
mutex_lock_nested(&inode->i_mutex, I_MUTEX_PARENT);
mutex_lock_nested(&child->i_mutex, I_MUTEX_CHILD);
/* Lockdep knows parent-before-child is valid */

/* Crossrelease annotation (lock released by different task) */
lock_acquire(&sem->dep_map, 0, 0, 0, 1, NULL, _RET_IP_);
lock_release(&sem->dep_map, _RET_IP_);

/* Assert lock is held (compile-time + runtime check) */
lockdep_assert_held(&my_lock);
lockdep_assert_held_write(&my_rwsem);
lockdep_assert_held_read(&my_rwsem);

/* Mark a lock as intentionally not validated */
lockdep_set_novalidate_class(&my_lock);
```

---

## 6. Common Lockdep Warnings

```mermaid
flowchart TD
    subgraph Warnings["Common Lockdep Reports"]
        W1["circular locking dependency<br/>A-B / B-A ordering violation"]
        W2["inconsistent lock state<br/>Lock used both with IRQs<br/>enabled and disabled"]
        W3["possible recursive locking<br/>Same lock class taken twice"]
        W4["possible irq lock inversion<br/>Lock X taken in IRQ but<br/>taken without irq-disable elsewhere"]
    end
    
    subgraph Fixes["How to Fix"]
        F1["Establish consistent lock ordering<br/>Always A before B everywhere"]
        F2["Always use spin_lock_irqsave<br/>if lock is used in IRQ handler"]
        F3["Use spin_lock_nested with subclass<br/>or define separate lock classes"]
        F4["Use spin_lock_irqsave for the lock<br/>since it is shared with IRQ context"]
    end
    
    W1 --> F1
    W2 --> F2
    W3 --> F3
    W4 --> F4

    style W1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style W2 fill:#e74c3c,stroke:#c0392b,color:#fff
    style W3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style W4 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 7. IRQ Inversion Detection

```c
/* Lockdep catches this subtle bug: */

/* Thread context: takes lock without disabling IRQs */
spin_lock(&my_lock);
/* ... critical section ... */
spin_unlock(&my_lock);

/* IRQ handler: also takes the same lock */
irqreturn_t my_handler(int irq, void *dev)
{
    spin_lock(&my_lock);  /* Lockdep: WARNING! */
    /* ... */
    spin_unlock(&my_lock);
    return IRQ_HANDLED;
}

/* Lockdep warning: inconsistent {HARDIRQ-ON-W} -> {IN-HARDIRQ-W}
 * 
 * Scenario: Thread holds lock, IRQ fires on same CPU,
 * IRQ handler tries to take same lock = DEADLOCK
 * 
 * Fix: use spin_lock_irqsave() in thread context */
```

---

## 8. Lockdep Performance and Limits

```c
/* Lockdep has kernel-wide limits: */
#define MAX_LOCKDEP_KEYS      8191   /* Lock classes */
#define MAX_LOCKDEP_CHAINS    65536  /* Lock chains */
#define MAX_LOCKDEP_ENTRIES   32768  /* Lock instances tracked */

/* When limits are hit:
 * "BUG: MAX_LOCKDEP_KEYS too low!"
 * Lockdep disables itself (no more checking)
 * 
 * Performance impact:
 * - ~10-30% slower lock acquisition
 * - Extra memory per lock class
 * - Only enable in development/testing, not production */
```

---

## 9. lockdep_assert_held — Defensive Programming

```c
/* Assert that a lock is held at a certain point */
void modify_list(struct my_device *dev)
{
    lockdep_assert_held(&dev->lock);
    /* If dev->lock is NOT held, BUG with stack trace */
    
    list_add(&new_entry->list, &dev->data_list);
}

/* For rwsem: */
void read_data(struct my_device *dev)
{
    lockdep_assert_held_read(&dev->rwsem);
    /* Asserts at least a read lock is held */
}

/* This catches callers that forget to take the lock */
```

---

## 10. Deep Q&A

### Q1: How does lockdep detect deadlocks that haven't happened yet?

**A:** Lockdep builds a directed graph of lock ordering: if lock A is ever held when lock B is acquired, an edge A→B is added. If later, B is held when A is acquired (B→A), lockdep detects the cycle A→B→A and reports it immediately. The actual deadlock requires two CPUs to hit the conflicting order simultaneously — which may be extremely rare. Lockdep catches the bug from a SINGLE observation of each ordering, even on different CPUs at different times.

### Q2: What is a lock chain and why does lockdep track them?

**A:** A lock chain is the sequence of locks held at a given point. For example, holding locks [A, B, C] is one chain, and [B, A] is another. Lockdep computes a hash of each unique chain and validates that no two chains create circular dependencies. This is more efficient than checking every individual edge — chains are cached and only validated once per unique combination.

### Q3: When should you use lock_class_key?

**A:** When locks are created dynamically and lockdep thinks they all have the same class (because they're initialized at the same code line). Example: each device instance has a lock — all are "same class" but have different valid orderings. Use `lockdep_set_class()` or `lockdep_set_class_and_name()` to give each group its own class, preventing false positive warnings.

### Q4: How do you fix a "possible recursive locking detected" warning?

**A:** This means the same lock CLASS is taken twice. Options: (1) If the locks are truly different (parent/child inodes), use `spin_lock_nested(&lock, SUBCLASS)` to tell lockdep they're different levels. (2) If it's the same lock taken twice, it's a real bug — restructure the code to avoid recursive locking. (3) Define separate `lock_class_key` for each level of the hierarchy.

---

[← Previous: 12 — Waitqueues](12_Waitqueues.md) | [Next: 14 — Priority Inversion and RT Mutexes →](14_Priority_Inversion_RT_Mutexes.md)
