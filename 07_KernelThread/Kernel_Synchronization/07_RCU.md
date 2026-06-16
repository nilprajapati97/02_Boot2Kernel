# 07 — RCU (Read-Copy-Update)

> **Scope**: RCU concepts, grace periods, rcu_read_lock/synchronize_rcu/call_rcu, RCU flavors (classic, SRCU, Tasks), RCU-protected data structures, rcu_dereference/rcu_assign_pointer, and internals.

---

## 1. RCU Core Concept

RCU allows **lock-free reads** and **deferred destruction** of shared data. Readers pay almost zero cost. Writers create a new version and wait for all pre-existing readers to finish before freeing the old version.

```mermaid
flowchart TD
    subgraph Key["RCU Key Idea"]
        K1["Readers: NO locks, NO atomic ops<br/>Just rcu_read_lock which disables preemption"]
        K2["Writers: Copy data, update pointer<br/>Then wait for all readers to finish"]
        K3["After grace period: free old data<br/>No reader can see it anymore"]
    end
    
    style K1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style K2 fill:#3498db,stroke:#2980b9,color:#fff
    style K3 fill:#f39c12,stroke:#e67e22,color:#fff
```

---

## 2. RCU Read-Copy-Update Sequence

```mermaid
sequenceDiagram
    participant R1 as Reader 1
    participant R2 as Reader 2
    participant PTR as Shared Pointer
    participant W as Writer

    Note over PTR: ptr points to Version A

    R1->>R1: rcu_read_lock
    R1->>PTR: rcu_dereference ptr - gets Version A
    R1->>R1: Reading Version A data...

    W->>W: Allocate new Version B
    W->>W: Copy A data to B, modify B
    W->>PTR: rcu_assign_pointer ptr = Version B
    Note over PTR: ptr now points to Version B

    R2->>R2: rcu_read_lock
    R2->>PTR: rcu_dereference ptr - gets Version B
    Note over R2: New reader sees Version B

    Note over R1: Still reading Version A<br/>This is fine and safe

    R1->>R1: rcu_read_unlock

    W->>W: synchronize_rcu - BLOCKS
    Note over W: Waits for ALL pre-existing<br/>RCU readers to finish<br/>R1 was the last one

    W->>W: kfree Version A
    Note over W: Safe: no reader can<br/>possibly reference A anymore
```

---

## 3. RCU API

```c
#include <linux/rcupdate.h>

/* --- Reader Side --- */
rcu_read_lock();
/* Marks entry to RCU read-side critical section.
 * On non-preemptible kernel: just preempt_disable()
 * On preemptible kernel: increments per-task nesting counter
 * FAST: ~1 instruction on non-PREEMPT kernels */

p = rcu_dereference(global_ptr);
/* Read the pointer with proper memory barrier.
 * On x86: just READ_ONCE() (no barrier needed, TSO)
 * On ARM: includes smp_read_barrier_depends() / consume */

/* ... use p to read data ... */

rcu_read_unlock();
/* FAST: just preempt_enable() or decrement counter */

/* --- Writer Side --- */
new = kmalloc(sizeof(*new), GFP_KERNEL);
*new = *old;          /* Copy old data */
new->field = value;    /* Modify */

rcu_assign_pointer(global_ptr, new);
/* Publish new version with write barrier.
 * smp_store_release(&global_ptr, new) */

synchronize_rcu();
/* BLOCK until all pre-existing RCU readers complete.
 * After this returns, no CPU can see old data. */

kfree(old);
/* Safely free old version */

/* --- Async freeing (callback after grace period) --- */
call_rcu(&old->rcu_head, my_rcu_callback);
/* Non-blocking. Callback invoked after grace period. */

void my_rcu_callback(struct rcu_head *head)
{
    struct my_data *old = container_of(head, struct my_data, rcu_head);
    kfree(old);
}

/* --- Convenience: kfree after grace period --- */
kfree_rcu(old, rcu_head);
/* Equivalent to call_rcu + kfree, less boilerplate */
```

---

## 4. Grace Period Explained

```mermaid
flowchart TD
    subgraph GP["Grace Period"]
        G1["Writer calls synchronize_rcu"]
        G2["RCU subsystem waits until<br/>every CPU has passed through<br/>a quiescent state"]
        G3["Quiescent state: CPU did one of:<br/>- context switch<br/>- entered idle<br/>- executed userspace<br/>- explicit rcu_quiescent_state"]
        G4["All CPUs passed quiescent state<br/>= Grace period complete"]
        G5["synchronize_rcu returns<br/>Old data can be freed"]
        
        G1 --> G2 --> G3 --> G4 --> G5
    end
    
    subgraph Why["Why This Works"]
        W1["rcu_read_lock disables preemption"]
        W2["So if a CPU context switches<br/>it CANNOT be in an RCU read section"]
        W3["Once ALL CPUs have context switched<br/>no reader can hold old data"]
    end
    
    style G1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style G4 fill:#2ecc71,stroke:#27ae60,color:#fff
    style G5 fill:#2ecc71,stroke:#27ae60,color:#fff
```

```mermaid
sequenceDiagram
    participant CPU0 as CPU 0
    participant CPU1 as CPU 1
    participant CPU2 as CPU 2
    participant RCU as RCU Subsystem

    Note over RCU: Grace Period starts

    CPU0->>CPU0: In RCU read section
    CPU1->>CPU1: Running user process
    CPU2->>CPU2: Idle

    Note over CPU1: Quiescent state: userspace
    Note over CPU2: Quiescent state: idle

    CPU0->>CPU0: rcu_read_unlock
    CPU0->>CPU0: Context switch
    Note over CPU0: Quiescent state: context switch

    RCU->>RCU: All CPUs passed quiescent state
    Note over RCU: GRACE PERIOD COMPLETE<br/>Callbacks can run<br/>Old data freed
```

---

## 5. RCU-Protected Linked List

```c
#include <linux/rculist.h>

struct my_entry {
    struct list_head list;
    struct rcu_head rcu;
    int key;
    char data[64];
};

static LIST_HEAD(my_list);
static DEFINE_SPINLOCK(list_lock); /* Protects writes */

/* Reader: lock-free traversal */
struct my_entry *find_entry(int key)
{
    struct my_entry *entry;
    
    rcu_read_lock();
    list_for_each_entry_rcu(entry, &my_list, list) {
        if (entry->key == key) {
            rcu_read_unlock();
            return entry;
        }
    }
    rcu_read_unlock();
    return NULL;
}

/* Writer: add entry */
void add_entry(struct my_entry *new)
{
    spin_lock(&list_lock);
    list_add_rcu(&new->list, &my_list);
    spin_unlock(&list_lock);
}

/* Writer: remove and free entry */
void remove_entry(struct my_entry *entry)
{
    spin_lock(&list_lock);
    list_del_rcu(&entry->list);
    spin_unlock(&list_lock);
    
    /* Wait for all readers traversing the list */
    synchronize_rcu();
    /* OR async: kfree_rcu(entry, rcu); */
    kfree(entry);
}

/* Writer: update entry (read-copy-update pattern) */
void update_entry(int key, const char *new_data)
{
    struct my_entry *old, *new;
    
    new = kmalloc(sizeof(*new), GFP_KERNEL);
    
    spin_lock(&list_lock);
    list_for_each_entry(old, &my_list, list) {
        if (old->key == key) {
            *new = *old;  /* Copy */
            strscpy(new->data, new_data, sizeof(new->data));
            list_replace_rcu(&old->list, &new->list);
            spin_unlock(&list_lock);
            kfree_rcu(old, rcu);
            return;
        }
    }
    spin_unlock(&list_lock);
    kfree(new);
}
```

---

## 6. RCU Flavors

```mermaid
flowchart TD
    subgraph Flavors["RCU Flavors"]
        CLASSIC["Classic RCU<br/>rcu_read_lock / synchronize_rcu<br/>Cannot sleep in read section<br/>Most common"]
        
        SRCU["SRCU - Sleepable RCU<br/>srcu_read_lock / synchronize_srcu<br/>CAN sleep in read section<br/>Uses per-CPU counters"]
        
        TASKS["Tasks RCU<br/>synchronize_rcu_tasks<br/>Waits for all voluntary<br/>context switches"]
        
        BH["RCU-bh<br/>rcu_read_lock_bh<br/>For softirq context<br/>Merged with classic in modern kernels"]
        
        SCHED["RCU-sched<br/>rcu_read_lock_sched<br/>For scheduler / hardirq<br/>Merged with classic in modern kernels"]
    end
    
    style CLASSIC fill:#2ecc71,stroke:#27ae60,color:#fff
    style SRCU fill:#3498db,stroke:#2980b9,color:#fff
    style TASKS fill:#f39c12,stroke:#e67e22,color:#fff
    style BH fill:#9b59b6,stroke:#8e44ad,color:#fff
    style SCHED fill:#e74c3c,stroke:#c0392b,color:#fff
```

### SRCU Example:

```c
#include <linux/srcu.h>

DEFINE_SRCU(my_srcu);

/* Reader — CAN SLEEP */
int idx = srcu_read_lock(&my_srcu);
/* ... may call sleeping functions ... */
srcu_read_unlock(&my_srcu, idx);

/* Writer */
synchronize_srcu(&my_srcu);
/* Waits for all SRCU readers of this domain */
```

---

## 7. rcu_dereference / rcu_assign_pointer Internals

```mermaid
flowchart TD
    subgraph Publish["rcu_assign_pointer - Publish"]
        P1["Writer prepares new data"]
        P2["smp_store_release: ensures all prior<br/>writes to new data are visible before<br/>the pointer update"]
        P3["Other CPUs see either old or new pointer<br/>Never a partially-constructed object"]
        P1 --> P2 --> P3
    end
    
    subgraph Subscribe["rcu_dereference - Subscribe"]
        S1["Reader loads pointer"]
        S2["On ARM/PowerPC: data dependency barrier<br/>Ensures data accessed through pointer<br/>is not speculatively loaded before pointer"]
        S3["On x86: just READ_ONCE<br/>x86 TSO provides this for free"]
        S1 --> S2 --> S3
    end
    
    subgraph Wrong["Common MISTAKE"]
        M1["DO NOT: p = global_ptr<br/>Compiler may reorder or cache"]
        M2["DO NOT: global_ptr = new<br/>CPU may reorder stores"]
        M3["ALWAYS use rcu_dereference<br/>and rcu_assign_pointer"]
    end
    
    style P2 fill:#2ecc71,stroke:#27ae60,color:#fff
    style S2 fill:#3498db,stroke:#2980b9,color:#fff
    style M1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style M2 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 8. synchronize_rcu vs call_rcu

```c
/* synchronize_rcu: BLOCKING */
void update_and_free(struct my_data *old, struct my_data *new)
{
    rcu_assign_pointer(global_ptr, new);
    synchronize_rcu();  /* Blocks for milliseconds */
    kfree(old);
    /* Simple but may cause latency spikes */
}

/* call_rcu: ASYNCHRONOUS */
void update_and_free_async(struct my_data *old, struct my_data *new)
{
    rcu_assign_pointer(global_ptr, new);
    call_rcu(&old->rcu_head, my_free_callback);
    /* Returns immediately. Callback runs after grace period. */
}

/* kfree_rcu: shorthand for call_rcu + kfree */
void update_and_free_easy(struct my_data *old, struct my_data *new)
{
    rcu_assign_pointer(global_ptr, new);
    kfree_rcu(old, rcu_head);
    /* Returns immediately. old freed after grace period. */
}
```

| | synchronize_rcu | call_rcu | kfree_rcu |
|--|-----------------|----------|-----------|
| Blocking? | YES | NO | NO |
| Latency | High (ms) | Very low | Very low |
| Memory | Immediate free | Deferred free | Deferred free |
| Callback needed? | No | Yes | No |
| Use when | Infrequent updates | Frequent updates | Just need kfree |

---

## 9. RCU in the Real Kernel

| Subsystem | RCU Usage |
|-----------|-----------|
| Networking | Route table lookup, socket hash tables, netfilter rules |
| VFS | Dentry cache (dcache) lookup, mount table |
| Scheduler | CPU hotplug, task list traversal |
| Device model | Driver list traversal, device list iteration |
| Security | SELinux policy lookup, credentials |
| Memory | VMAs in mmap_lock changes (5.19+ maple tree) |

---

## 10. Deep Q&A

### Q1: Why is RCU so much faster than rwlock for readers?

**A:** `rcu_read_lock()` on non-PREEMPT kernels is literally `preempt_disable()` — a single per-CPU counter increment, no atomic operations, no cache-line bouncing between CPUs. `rwlock_t` requires an atomic read-modify-write on a shared cache line, causing cache coherency traffic (MESI) across all CPUs. On a 64-core system, rwlock reader acquisition can be 100x slower than RCU.

### Q2: What happens if rcu_read_lock is held too long?

**A:** Grace periods cannot complete until all readers finish. Long readers delay `synchronize_rcu()` and the freeing of old data, causing memory accumulation. The kernel has `CONFIG_RCU_CPU_STALL_TIMEOUT` (default 21 seconds) — if a CPU hasn't passed a quiescent state for that long, a warning is printed. Extremely long readers can trigger OOM because callbacks pile up.

### Q3: Can you nest rcu_read_lock?

**A:** Yes, nesting is fully supported. `rcu_read_lock()` increments a counter and `rcu_read_unlock()` decrements it. The critical section ends only when the outermost unlock runs and the counter reaches zero. This is critical because helper functions may use `rcu_read_lock()` internally, and callers may also hold it.

### Q4: How does PREEMPT_RCU differ from classic RCU?

**A:** Classic RCU (non-preemptible): `rcu_read_lock()` disables preemption, so a context switch implies no RCU reader. PREEMPT_RCU (CONFIG_PREEMPT): readers CAN be preempted. The RCU subsystem tracks per-task nesting and uses a boosting mechanism to ensure grace periods complete. PREEMPT_RCU is more complex but necessary for low-latency RT kernels.

### Q5: Explain the difference between list_for_each_entry and list_for_each_entry_rcu.

**A:** `list_for_each_entry_rcu()` wraps the pointer load with `rcu_dereference()`, which inserts necessary memory barriers so the CPU reads the node data AFTER reading the pointer to the node. `list_for_each_entry()` uses plain pointer loads — safe under locks but not under RCU because of potential reordering on weakly-ordered architectures (ARM, PowerPC).

---

[← Previous: 06 — RWLocks and SeqLocks](06_RWLocks_and_SeqLocks.md) | [Next: 08 — Completions →](08_Completions.md)
