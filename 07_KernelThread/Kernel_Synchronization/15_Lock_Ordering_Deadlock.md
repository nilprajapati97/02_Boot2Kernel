# 15 — Lock Ordering and Deadlock Prevention

> **Scope**: ABBA deadlocks, lock hierarchies, consistent ordering rules, trylock patterns, lock ordering in VFS/networking/block layer, and practical strategies for deadlock avoidance.

---

## 1. The ABBA Deadlock

```mermaid
sequenceDiagram
    participant CPU0 as CPU 0
    participant LA as Lock A
    participant LB as Lock B
    participant CPU1 as CPU 1

    CPU0->>LA: lock A - acquired
    CPU1->>LB: lock B - acquired
    
    CPU0->>LB: lock B - BLOCKED
    Note over CPU0: Waiting for CPU 1 to release B
    
    CPU1->>LA: lock A - BLOCKED
    Note over CPU1: Waiting for CPU 0 to release A
    
    Note over CPU0,CPU1: DEADLOCK<br/>Neither can make progress<br/>Both wait forever
```

```c
/* BROKEN: inconsistent lock ordering */

/* Thread 1: */           /* Thread 2: */
spin_lock(&lock_a);       spin_lock(&lock_b);
spin_lock(&lock_b);       spin_lock(&lock_a);  /* DEADLOCK */
/* ... */                 /* ... */
spin_unlock(&lock_b);     spin_unlock(&lock_a);
spin_unlock(&lock_a);     spin_unlock(&lock_b);

/* FIXED: consistent ordering — always A before B */

/* Thread 1: */           /* Thread 2: */
spin_lock(&lock_a);       spin_lock(&lock_a);
spin_lock(&lock_b);       spin_lock(&lock_b);
/* ... */                 /* ... */
spin_unlock(&lock_b);     spin_unlock(&lock_b);
spin_unlock(&lock_a);     spin_unlock(&lock_a);
```

---

## 2. Lock Ordering Hierarchy

```mermaid
flowchart TD
    subgraph Hierarchy["Lock Ordering Hierarchy"]
        L1["Level 1: Global / Subsystem locks"]
        L2["Level 2: Per-device locks"]
        L3["Level 3: Per-object locks"]
        L4["Level 4: Per-field / leaf locks"]
        
        L1 --> L2 --> L3 --> L4
    end
    
    subgraph Rule["RULE: Always lock from TOP to BOTTOM"]
        R1["Lock Level 1 FIRST"]
        R2["Then Level 2"]
        R3["Then Level 3"]
        R4["NEVER reverse the order"]
        
        R1 --> R2 --> R3
    end
    
    subgraph Example["VFS Example"]
        E1["1. inode->i_rwsem parent"]
        E2["2. inode->i_rwsem child"]
        E3["3. page lock"]
        E4["4. mapping->i_mmap_rwsem"]
    end
    
    style R4 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 3. Real Kernel Lock Ordering Rules

### VFS Lock Ordering:

```c
/*
 * VFS lock ordering (from Documentation/filesystems/locking.rst):
 *
 * 1. rename_lock (global rw seqlock)
 * 2. dentry->d_lock (per-dentry spinlock)
 * 3. inode->i_lock
 * 4. inode->i_rwsem
 *    - I_MUTEX_PARENT (parent directory)
 *    - I_MUTEX_CHILD  (child inode)
 *    - I_MUTEX_XATTR  (xattr operations)
 *    - I_MUTEX_NORMAL (normal operations)
 * 5. mapping->invalidate_lock
 * 6. page lock (lock_page)
 * 7. mapping->i_mmap_rwsem
 *
 * rename(): locks parent directories before children
 * Always: lock parent inode before child inode
 * Two directories: lock by inode number (lower first)
 */
```

### Network Stack:

```c
/*
 * Network lock ordering:
 * 1. rtnl_lock() (global network configuration mutex)
 * 2. dev->addr_list_lock
 * 3. sk->sk_lock (socket lock)
 * 4. sk->sk_receive_queue.lock
 *
 * Always take rtnl_lock before any per-device lock.
 */
```

---

## 4. Deadlock Prevention Strategies

```mermaid
flowchart TD
    subgraph Strategies["Deadlock Prevention"]
        S1["Consistent Ordering<br/>Define a global order<br/>Always follow it"]
        S2["Lock Numbering<br/>For same-class locks:<br/>lock lower-numbered first"]
        S3["Trylock Pattern<br/>Try second lock, if fail:<br/>release first, retry"]
        S4["Single Lock<br/>Coarser granularity<br/>One lock instead of two"]
        S5["Lock-Free<br/>Use RCU, atomics, per-CPU<br/>Eliminate lock entirely"]
    end
    
    S1 --> |"Most common"| NOTE1["Document and enforce<br/>the ordering rules"]
    S2 --> NOTE2["Used for inodes:<br/>lock lower ino first"]
    S3 --> NOTE3["Used when ordering<br/>is impossible to determine"]
    
    style S1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style S3 fill:#3498db,stroke:#2980b9,color:#fff
    style S5 fill:#f39c12,stroke:#e67e22,color:#fff
```

---

## 5. Trylock Pattern for Dynamic Ordering

```c
/* When you cannot determine lock order at compile time:
 * Example: locking two arbitrary inodes */

void lock_two_inodes(struct inode *inode1, struct inode *inode2)
{
    /* Strategy: always lock by inode number */
    if (inode1->i_ino < inode2->i_ino) {
        mutex_lock(&inode1->i_mutex);
        mutex_lock_nested(&inode2->i_mutex, I_MUTEX_CHILD);
    } else if (inode1->i_ino > inode2->i_ino) {
        mutex_lock(&inode2->i_mutex);
        mutex_lock_nested(&inode1->i_mutex, I_MUTEX_CHILD);
    } else {
        /* Same inode — lock once */
        mutex_lock(&inode1->i_mutex);
    }
}

/* Alternative: trylock + retry pattern */
void lock_two_locks(spinlock_t *a, spinlock_t *b)
{
    for (;;) {
        spin_lock(a);
        if (spin_trylock(b))
            return;  /* Got both */
        spin_unlock(a);
        
        /* Swap so we try the other order next */
        swap(a, b);
        cpu_relax();
    }
}
```

---

## 6. Deadlock Types

```mermaid
flowchart TD
    subgraph Types["Types of Deadlock"]
        subgraph Self["Self Deadlock"]
            SD1["Task takes lock A"]
            SD2["Task tries lock A again"]
            SD3["DEADLOCK: waiting for itself"]
            SD1 --> SD2 --> SD3
        end
        
        subgraph ABBA2["ABBA Deadlock"]
            AB1["CPU0: A then B"]
            AB2["CPU1: B then A"]
            AB3["DEADLOCK: circular wait"]
            AB1 --> AB3
            AB2 --> AB3
        end
        
        subgraph IRQ_DL["IRQ Deadlock"]
            ID1["Process: spin_lock A"]
            ID2["IRQ fires on same CPU"]
            ID3["IRQ: spin_lock A"]
            ID4["DEADLOCK: IRQ waits<br/>but process cannot run"]
            ID1 --> ID2 --> ID3 --> ID4
        end
        
        subgraph Chain["Chain Deadlock"]
            CD1["T1: A then B"]
            CD2["T2: B then C"]
            CD3["T3: C then A"]
            CD4["DEADLOCK: cycle A-B-C-A"]
            CD1 --> CD4
            CD2 --> CD4
            CD3 --> CD4
        end
    end
    
    style SD3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style AB3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style ID4 fill:#e74c3c,stroke:#c0392b,color:#fff
    style CD4 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 7. Lock Ordering Documentation Pattern

```c
/*
 * Lock ordering for struct my_subsystem:
 *
 *   global_lock
 *     -> dev->config_lock
 *       -> dev->data_lock
 *         -> dev->irq_lock (irqsave)
 *
 * Rules:
 * 1. global_lock protects device list
 * 2. config_lock protects configuration changes
 * 3. data_lock protects runtime data
 * 4. irq_lock is taken in IRQ context (must use irqsave)
 * 5. NEVER take global_lock while holding data_lock
 * 6. IRQ handler only takes irq_lock
 */

struct my_device {
    struct mutex config_lock;    /* Level 2 */
    spinlock_t data_lock;        /* Level 3 */
    spinlock_t irq_lock;         /* Level 4 — used in IRQ */
};

static DEFINE_MUTEX(global_lock); /* Level 1 */
```

---

## 8. Detecting Deadlocks

```mermaid
flowchart TD
    subgraph Detection["Deadlock Detection Tools"]
        D1["Lockdep<br/>CONFIG_PROVE_LOCKING<br/>Detects ordering violations<br/>at runtime"]
        D2["Hung Task Detector<br/>CONFIG_DETECT_HUNG_TASK<br/>Reports tasks stuck in D state<br/>over 120 seconds"]
        D3["Soft Lockup Detector<br/>CONFIG_SOFTLOCKUP_DETECTOR<br/>Detects CPU stuck in kernel"]
        D4["Lock Stat<br/>CONFIG_LOCK_STAT<br/>Lock contention statistics"]
        D5["RCU Stall Detector<br/>Reports CPUs stuck in RCU<br/>read sections"]
    end
    
    subgraph Debug["Debug Commands"]
        DB1["echo t > /proc/sysrq-trigger<br/>Show all task states"]
        DB2["cat /proc/lockdep<br/>Lock dependency list"]
        DB3["cat /proc/lockdep_chains<br/>Lock chain list"]
        DB4["cat /proc/lock_stat<br/>Contention statistics"]
    end
    
    style D1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style D2 fill:#3498db,stroke:#2980b9,color:#fff
```

---

## 9. Deep Q&A

### Q1: How do you determine the correct lock ordering for a new subsystem?

**A:** (1) Identify all locks in the subsystem and their scope (global, per-device, per-object). (2) Map the call graph: which functions can call which other functions while holding locks. (3) Assign levels: coarser/higher-scope locks get lower numbers. (4) Rule: always acquire lower-numbered locks first. (5) Document the ordering in a comment at the top of the source file. (6) Use lockdep annotations (nested, subclass) to validate at runtime.

### Q2: What if you need to lock two objects of the same type?

**A:** Use a deterministic tiebreaker: (1) Sort by memory address (`if (a < b) lock(a), lock(b)`). (2) Sort by a unique ID (inode number, device number). (3) Use trylock with retry. The kernel uses inode number ordering for directory operations like rename. Address ordering is used for memory management (mmap_lock ordering for mremap).

### Q3: Can you detect deadlocks in production without lockdep?

**A:** Partially. The hung task detector (`/proc/sys/kernel/hung_task_timeout_secs`) triggers after a configurable timeout (default 120s) if a task stays in TASK_UNINTERRUPTIBLE. The soft lockup detector watches for CPUs that don't schedule for 20+ seconds. `SysRq+t` dumps all task stack traces for manual analysis. But none of these are as precise as lockdep's compile-path analysis.

### Q4: Explain the ABBA problem in the VFS rename operation.

**A:** `rename(old_dir/A, new_dir/B)` needs to lock both parent directories (`old_dir` and `new_dir`). If two renames happen simultaneously in opposite directions, one locks `old_dir` then `new_dir`, the other locks `new_dir` then `old_dir` — classic ABBA. The VFS solves this by always locking by inode number (lower number first). The `lock_rename()` function handles this with `mutex_lock_nested()` and proper subclass annotations.

---

[← Previous: 14 — Priority Inversion and RT Mutexes](14_Priority_Inversion_RT_Mutexes.md) | [Next: 16 — Futex: User-Kernel Synchronization →](16_Futex_User_Kernel_Sync.md)
