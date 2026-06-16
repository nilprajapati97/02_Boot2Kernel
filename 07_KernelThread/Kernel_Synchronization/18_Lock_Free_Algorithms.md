# 18 — Lock-Free Algorithms in the Linux Kernel

> **Scope**: Lock-free vs wait-free, RCU-based lock-free patterns, lockless queues (kfifo), atomic pointer operations, cmpxchg loops, hazard pointers concept, and kernel lock-free data structures.

---

## 1. Lock-Free Terminology

```mermaid
flowchart TD
    subgraph Terms["Lock-Free Spectrum"]
        BL["Blocking<br/>Uses locks: mutex, spinlock<br/>One stuck thread blocks everyone"]
        LF["Lock-Free<br/>No locks. At least ONE thread<br/>makes progress at all times<br/>Others may retry"]
        WF["Wait-Free<br/>No locks. EVERY thread<br/>completes in bounded steps<br/>Strongest guarantee"]
        OBS["Obstruction-Free<br/>A thread makes progress<br/>if no other thread<br/>runs concurrently"]
    end
    
    BL -->|"Stronger"| OBS -->|"Stronger"| LF -->|"Strongest"| WF

    style BL fill:#e74c3c,stroke:#c0392b,color:#fff
    style LF fill:#3498db,stroke:#2980b9,color:#fff
    style WF fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 2. The cmpxchg Loop — Foundation of Lock-Free

```c
/* Compare-and-exchange: the core atomic primitive */
/* atomic_cmpxchg(ptr, expected, new):
 *   if (*ptr == expected) { *ptr = new; return expected; }
 *   else { return *ptr; }
 *   ALL done atomically in ONE instruction */

/* Lock-free increment pattern: */
void lock_free_increment(atomic_t *counter)
{
    int old, new;
    do {
        old = atomic_read(counter);
        new = old + 1;
    } while (atomic_cmpxchg(counter, old, new) != old);
    /* If someone changed counter between read and cmpxchg,
     * cmpxchg fails, we retry with the new value.
     * GUARANTEED: at least one thread succeeds per round. */
}
```

```mermaid
sequenceDiagram
    participant T1 as Thread 1
    participant VAL as counter=5
    participant T2 as Thread 2

    T1->>VAL: Read: old=5
    T2->>VAL: Read: old=5
    
    T1->>VAL: cmpxchg 5 to 6 - SUCCESS
    Note over VAL: counter=6
    
    T2->>VAL: cmpxchg 5 to 6 - FAILS old value changed
    Note over T2: Retry: read=6
    T2->>VAL: cmpxchg 6 to 7 - SUCCESS
    Note over VAL: counter=7
```

---

## 3. RCU — The Kernel's Primary Lock-Free Mechanism

```mermaid
flowchart TD
    subgraph RCU_LF["RCU as Lock-Free Read"]
        R1["Readers: NO locks, NO atomics<br/>Just rcu_read_lock + rcu_dereference"]
        R2["Writers: Atomic pointer update<br/>rcu_assign_pointer"]
        R3["Old data freed after grace period"]
        R4["Readers NEVER block or retry<br/>This is technically WAIT-FREE reads"]
    end
    
    subgraph Patterns["Lock-Free Patterns Using RCU"]
        P1["RCU protected linked list<br/>list_for_each_entry_rcu"]
        P2["RCU hash table<br/>hlist_for_each_entry_rcu"]
        P3["RCU protected radix tree"]
        P4["RCU protected singly-linked list"]
    end
    
    style R4 fill:#2ecc71,stroke:#27ae60,color:#fff
```

### Lock-Free List Update:

```c
/* Lock-free insertion (RCU): */
void rcu_list_add(struct item *new, struct list_head *head)
{
    /* No lock needed for the publish step — it's atomic */
    list_add_rcu(&new->list, head);
    /* rcu_assign_pointer ensures new node's fields are
     * visible before the list pointer is updated */
}

/* Reader traversal — completely lock-free: */
struct item *find(int key)
{
    struct item *item;
    rcu_read_lock();
    list_for_each_entry_rcu(item, &my_list, list) {
        if (item->key == key) {
            rcu_read_unlock();
            return item;
        }
    }
    rcu_read_unlock();
    return NULL;
}
```

---

## 4. kfifo — Lock-Free Ring Buffer

```c
#include <linux/kfifo.h>

/* kfifo: single-producer, single-consumer lock-free FIFO */
DEFINE_KFIFO(my_fifo, struct event, 256);  /* power-of-2 size */

/* Producer (one thread only): */
struct event e = { .type = EVENT_DATA, .value = 42 };
kfifo_put(&my_fifo, e);
/* No lock needed! Uses memory barriers internally */

/* Consumer (one thread only): */
struct event e;
if (kfifo_get(&my_fifo, &e))
    process_event(&e);
/* No lock needed! */
```

```mermaid
flowchart TD
    subgraph KFIFO["kfifo Lock-Free FIFO"]
        subgraph Structure["Internal"]
            BUF["Buffer: power-of-2 array"]
            IN["in index: only producer writes"]
            OUT["out index: only consumer writes"]
        end
        
        subgraph HowItWorks["How It Works"]
            H1["Producer writes data at buffer IN mod size"]
            H2["smp_wmb: ensure data write before index update"]
            H3["Producer increments IN"]
            H4["Consumer reads OUT, checks IN"]
            H5["smp_rmb: ensure index read before data read"]
            H6["Consumer reads data at buffer OUT mod size"]
            H7["Consumer increments OUT"]
        end
        
        H1 --> H2 --> H3
        H4 --> H5 --> H6 --> H7
    end
    
    style H2 fill:#f39c12,stroke:#e67e22,color:#fff
    style H5 fill:#3498db,stroke:#2980b9,color:#fff
```

```c
/* kfifo internal implementation (simplified): */
struct kfifo {
    unsigned int in;     /* Write index (producer only) */
    unsigned int out;    /* Read index (consumer only) */
    unsigned int mask;   /* size - 1 (power of 2) */
    void *data;
};

bool kfifo_put(struct kfifo *fifo, const void *val)
{
    unsigned int off = fifo->in & fifo->mask;
    
    if (kfifo_is_full(fifo))
        return false;
    
    memcpy(fifo->data + off * elem_size, val, elem_size);
    
    smp_wmb();  /* Data visible before index update */
    fifo->in++;
    return true;
}

bool kfifo_get(struct kfifo *fifo, void *val)
{
    unsigned int off;
    
    if (kfifo_is_empty(fifo))
        return false;
    
    smp_rmb();  /* Index read before data read */
    off = fifo->out & fifo->mask;
    memcpy(val, fifo->data + off * elem_size, elem_size);
    
    fifo->out++;
    return true;
}
```

---

## 5. Lock-Free Stack (Treiber Stack)

```c
/* Classic lock-free stack using cmpxchg */
struct node {
    void *data;
    struct node *next;
};

struct lock_free_stack {
    struct node *head;  /* Atomic pointer */
};

void push(struct lock_free_stack *s, struct node *new)
{
    struct node *old_head;
    do {
        old_head = READ_ONCE(s->head);
        new->next = old_head;
    } while (cmpxchg(&s->head, old_head, new) != old_head);
}

struct node *pop(struct lock_free_stack *s)
{
    struct node *old_head;
    struct node *new_head;
    do {
        old_head = READ_ONCE(s->head);
        if (!old_head)
            return NULL;
        new_head = old_head->next;
    } while (cmpxchg(&s->head, old_head, new_head) != old_head);
    return old_head;
    /* WARNING: ABA problem exists — need RCU or epoch for safe free */
}
```

---

## 6. The ABA Problem

```mermaid
sequenceDiagram
    participant T1 as Thread 1
    participant HEAD as Stack Head=A
    participant T2 as Thread 2

    T1->>HEAD: Pop: read head=A, next=B
    Note over T1: About to cmpxchg A to B

    T2->>HEAD: Pop A - head becomes B
    T2->>HEAD: Pop B - head becomes C
    T2->>HEAD: Push A back - head becomes A
    Note over HEAD: Head is A again but<br/>A next is now different

    T1->>HEAD: cmpxchg A to B - SUCCEEDS
    Note over T1: BUG: B was already popped<br/>Stack is now corrupted

    Note over T1,T2: SOLUTION: Use RCU for deferred freeing<br/>or tagged pointers with generation counter
```

```c
/* ABA solution in Linux: use RCU */
struct node *pop(struct lock_free_stack *s)
{
    struct node *old_head;
    struct node *new_head;
    
    rcu_read_lock();
    do {
        old_head = rcu_dereference(s->head);
        if (!old_head) {
            rcu_read_unlock();
            return NULL;
        }
        new_head = READ_ONCE(old_head->next);
    } while (cmpxchg(&s->head, old_head, new_head) != old_head);
    rcu_read_unlock();
    
    /* Don't free old_head immediately — other threads may reference it */
    /* Caller must: kfree_rcu(old_head, rcu) */
    return old_head;
}
```

---

## 7. llist — Lock-Free Singly Linked List

```c
#include <linux/llist.h>

/* Linux provides a lock-free singly-linked list */
struct llist_head my_list = LLIST_HEAD_INIT(my_list);

struct my_item {
    struct llist_node node;
    int data;
};

/* Add (lock-free, multiple producers OK): */
struct my_item *item = kmalloc(sizeof(*item), GFP_KERNEL);
item->data = 42;
llist_add(&item->node, &my_list);
/* Uses cmpxchg loop internally */

/* Delete all (single consumer): */
struct llist_node *list = llist_del_all(&my_list);
/* Atomically removes entire list, returns old head */

/* Process removed items: */
struct llist_node *pos;
llist_for_each(pos, list) {
    struct my_item *item = llist_entry(pos, struct my_item, node);
    process(item);
    kfree(item);
}
```

---

## 8. When to Use Lock-Free vs Locks

```mermaid
flowchart TD
    Q1{"Performance critical<br/>hot path?"}
    Q1 -- No --> LOCK["Use locks<br/>Simpler, correct, maintainable"]
    Q1 -- Yes --> Q2{"Read-heavy?"}
    Q2 -- Yes --> RCU["Use RCU<br/>Best for read-mostly data"]
    Q2 -- No --> Q3{"Single producer,<br/>single consumer?"}
    Q3 -- Yes --> KFIFO["Use kfifo<br/>Lock-free ring buffer"]
    Q3 -- No --> Q4{"Simple add-to-list<br/>batch-remove pattern?"}
    Q4 -- Yes --> LLIST["Use llist<br/>Lock-free linked list"]
    Q4 -- No --> LOCK2["Use fine-grained locks<br/>Lock-free is too complex"]
    
    style LOCK fill:#2ecc71,stroke:#27ae60,color:#fff
    style RCU fill:#3498db,stroke:#2980b9,color:#fff
    style KFIFO fill:#f39c12,stroke:#e67e22,color:#fff
    style LOCK2 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 9. Deep Q&A

### Q1: Why is lock-free programming hard?

**A:** (1) Memory ordering: without locks, you must manually reason about CPU and compiler reordering. (2) ABA problem: detecting that data changed and changed back requires extra mechanisms. (3) Memory reclamation: you can't free nodes while other threads may reference them — need RCU, hazard pointers, or epoch-based reclamation. (4) Testing: bugs manifest only under specific timing — hard to reproduce.

### Q2: Is RCU truly lock-free?

**A:** RCU readers are actually **wait-free** — they complete in constant time (just preempt_disable/enable) regardless of what other threads do. RCU writers use locks to serialize updates, but readers never block on those locks. So: readers are wait-free, the overall system (read+write) is lock-free (readers always make progress, writers may be serialized).

### Q3: When does kfifo need external locking?

**A:** kfifo is lock-free ONLY for single-producer/single-consumer. If you have multiple producers, you must serialize them with a spinlock. If you have multiple consumers, same. For MPMC (multi-producer/multi-consumer), use `kfifo_in_spinlocked()` and `kfifo_out_spinlocked()` which take an external spinlock.

### Q4: What is the performance advantage of lock-free?

**A:** Lock-free eliminates: (1) Cache-line bouncing from lock acquisition (shared counter). (2) Convoying: all threads serialize behind one lock holder. (3) Priority inversion: no lock to cause it. (4) Context switch overhead: no sleeping. Benchmarks show 2-10x throughput improvement for read-heavy workloads (RCU vs rwlock). For write-heavy workloads, the advantage is smaller or may even be negative due to retry overhead.

---

[← Previous: 17 — Spinlock Variants: BH, IRQ, Nested](17_Spinlock_Variants_BH_IRQ.md) | [Next: 19 — Synchronization in Interrupt Context →](19_Sync_in_Interrupt_Context.md)
