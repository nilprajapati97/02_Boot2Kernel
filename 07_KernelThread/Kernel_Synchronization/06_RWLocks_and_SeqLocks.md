# 06 — Read-Write Locks and SeqLocks

> **Scope**: rwlock_t, rwlock vs rwsem, seqlock, seqcount, reader-writer fairness, writer starvation, and choosing the right read-write primitive.

---

## 1. Read-Write Problem Overview

Many data structures are read far more often than written. A single mutex serializes all access — even readers blocking other readers. Read-write locks solve this.

```mermaid
flowchart TD
    subgraph Problem["The Problem"]
        P1["Data: read 95%, write 5%"]
        P2["Mutex: all 100% serialized"]
        P3["Waste: readers block readers unnecessarily"]
    end
    
    subgraph Solution["Read-Write Locks"]
        S1["Multiple readers: concurrent access"]
        S2["Writer: exclusive access"]
        S3["Reader + Writer: writer waits for readers"]
    end
    
    subgraph Types["Linux Primitives"]
        T1["rwlock_t - spinning RW lock"]
        T2["rw_semaphore - sleeping RW lock"]
        T3["seqlock_t - optimistic RW lock"]
        T4["seqcount_t - lightweight sequence counter"]
    end
    
    Problem --> Solution --> Types

    style S1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style S2 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 2. rwlock_t — Spinning Read-Write Lock

```c
#include <linux/rwlock.h>

rwlock_t my_rwlock;
rwlock_init(&my_rwlock);

/* OR static initialization */
DEFINE_RWLOCK(my_rwlock);

/* Reader (shared) — multiple concurrent */
read_lock(&my_rwlock);
/* ... read shared data ... */
read_unlock(&my_rwlock);

/* Writer (exclusive) — blocks all readers and writers */
write_lock(&my_rwlock);
/* ... modify shared data ... */
write_unlock(&my_rwlock);
```

### Variants:

```c
/* With IRQ save */
read_lock_irqsave(&rwlock, flags);
read_unlock_irqrestore(&rwlock, flags);

write_lock_irqsave(&rwlock, flags);
write_unlock_irqrestore(&rwlock, flags);

/* With bottom-half disable */
read_lock_bh(&rwlock);
write_lock_bh(&rwlock);

/* Non-blocking try */
if (read_trylock(&rwlock)) { /* ... */ read_unlock(&rwlock); }
if (write_trylock(&rwlock)) { /* ... */ write_unlock(&rwlock); }
```

---

## 3. rwlock_t Internal Mechanism

```mermaid
sequenceDiagram
    participant R1 as Reader 1
    participant R2 as Reader 2
    participant LOCK as rwlock count=0
    participant W as Writer

    R1->>LOCK: read_lock - count becomes 1
    R2->>LOCK: read_lock - count becomes 2
    Note over LOCK: Two readers concurrently

    W->>LOCK: write_lock - count is not 0
    Note over W: SPINNING waiting for<br/>all readers to finish

    R1->>LOCK: read_unlock - count becomes 1
    R2->>LOCK: read_unlock - count becomes 0

    W->>LOCK: count is 0, acquire write lock
    Note over LOCK: WRITE LOCK HELD<br/>count = -1 or special flag

    R1->>LOCK: read_lock - write locked
    Note over R1: SPINNING waiting for writer
    
    W->>LOCK: write_unlock
    R1->>LOCK: read_lock succeeds
```

---

## 4. rwlock_t Problems — Writer Starvation

```mermaid
flowchart TD
    subgraph Starvation["Writer Starvation Problem"]
        S1["Writer wants write_lock"]
        S2["Reader 1 is holding read_lock"]
        S3["Reader 2 arrives - read_lock succeeds<br/>new readers can STILL enter"]
        S4["Reader 3 arrives - read_lock succeeds<br/>continuous reader stream"]
        S5["Writer NEVER gets to write<br/>because readers keep arriving"]
    end
    
    S1 --> S2 --> S3 --> S4 --> S5
    S5 -.->|"Solution: use rw_semaphore<br/>which has writer preference<br/>or use seqlock"| FIX["rw_semaphore or seqlock"]
    
    style S5 fill:#e74c3c,stroke:#c0392b,color:#fff
    style FIX fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 5. SeqLock — Optimistic Reader Lock

SeqLock is designed for data that is **read very frequently** and **written very rarely**. Readers NEVER block writers. Writers NEVER block readers. Readers just retry if they read during a write.

```mermaid
flowchart TD
    subgraph SeqLock["SeqLock Concept"]
        direction LR
        SEQ["Sequence Counter<br/>Even = stable<br/>Odd = write in progress"]
        
        subgraph Writer["Writer Path"]
            W1["Increment seq to ODD"]
            W2["Modify data"]
            W3["Increment seq to EVEN"]
        end
        
        subgraph Reader["Reader Path"]
            R1["Read seq START"]
            R2["Read data"]
            R3["Read seq END"]
            R4{"START == END<br/>AND even?"}
            R5Y["YES: data is valid, use it"]
            R5N["NO: retry from R1"]
            R1 --> R2 --> R3 --> R4
            R4 -- Yes --> R5Y
            R4 -- No --> R5N
            R5N -->|"Loop"| R1
        end
    end
    
    style W1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style W3 fill:#2ecc71,stroke:#27ae60,color:#fff
    style R5Y fill:#2ecc71,stroke:#27ae60,color:#fff
    style R5N fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 6. SeqLock API

```c
#include <linux/seqlock.h>

/* Full seqlock (spinlock + seqcount) */
seqlock_t my_seqlock;
seqlock_init(&my_seqlock);

/* Writer: exclusive, takes a spinlock */
write_seqlock(&my_seqlock);
/* ... modify data ... */
write_sequnlock(&my_seqlock);

/* Reader: optimistic, NEVER blocks */
unsigned int seq;
do {
    seq = read_seqbegin(&my_seqlock);
    /* ... read data into local variables ... */
} while (read_seqretry(&my_seqlock, seq));
```

### Sequence Diagram:

```mermaid
sequenceDiagram
    participant R as Reader
    participant SEQ as seqlock seq=0
    participant W as Writer

    R->>SEQ: read_seqbegin - seq=0
    R->>R: Read data into local vars

    Note over W: Writer arrives mid-read
    W->>SEQ: write_seqlock - seq becomes 1 odd
    W->>W: Modify shared data
    W->>SEQ: write_sequnlock - seq becomes 2 even

    R->>SEQ: read_seqretry - start was 0, now is 2
    Note over R: MISMATCH - data may be torn<br/>Retry the read

    R->>SEQ: read_seqbegin - seq=2
    R->>R: Read data again
    R->>SEQ: read_seqretry - start was 2, still 2
    Note over R: MATCH and EVEN - data is valid
```

---

## 7. seqcount_t — Lightweight Variant

```c
/* seqcount_t: sequence counter WITHOUT embedded spinlock.
 * You provide your own serialization for writers. */

seqcount_t my_seq;
seqcount_init(&my_seq);

/* Writer: serialize yourself (e.g., with existing spinlock) */
spin_lock(&my_lock);
write_seqcount_begin(&my_seq);
/* ... modify data ... */
write_seqcount_end(&my_seq);
spin_unlock(&my_lock);

/* Reader: same as seqlock */
unsigned int seq;
do {
    seq = read_seqcount_begin(&my_seq);
    /* ... read data ... */
} while (read_seqcount_retry(&my_seq, seq));
```

### Typed seqcount (Linux 5.10+):

```c
/* seqcount associated with specific lock type */
seqcount_spinlock_t my_seq;
seqcount_rwlock_t   my_seq2;
seqcount_mutex_t    my_seq3;
/* Lockdep validates you hold the correct lock when writing */
```

---

## 8. Real-World: jiffies and xtime

```c
/* The most famous seqlock in Linux: jiffies/xtime update */

/* kernel/time/timekeeping.c */
static seqcount_raw_spinlock_t tk_core_seq;

/* Writer: timer interrupt updates time (very rare) */
void timekeeping_advance(void)
{
    raw_spin_lock(&tk_core.lock);
    write_seqcount_begin(&tk_core_seq);
    
    /* Update nanosecond clock, jiffies, etc. */
    tk->tkr_mono.cycle_last = cycle_now;
    tk->xtime_sec += seconds;
    
    write_seqcount_end(&tk_core_seq);
    raw_spin_unlock(&tk_core.lock);
}

/* Reader: ktime_get() called MILLIONS of times per second */
ktime_t ktime_get(void)
{
    unsigned int seq;
    s64 secs, nsecs;
    
    do {
        seq = read_seqcount_begin(&tk_core_seq);
        secs = tk->xtime_sec;
        nsecs = timekeeping_get_ns();
    } while (read_seqcount_retry(&tk_core_seq, seq));
    
    return ktime_set(secs, nsecs);
}
/* Readers NEVER block — perfect for hot-path time queries */
```

---

## 9. Comparison Matrix

```mermaid
flowchart TD
    Q1{"Shared data?"} --> Q2{"Read-heavy workload?"}
    Q2 -- No --> MUTEX["Use mutex or spinlock"]
    Q2 -- Yes --> Q3{"Can readers tolerate retry?"}
    Q3 -- Yes --> Q4{"Writes very rare?"}
    Q4 -- Yes --> SEQLOCK["seqlock / seqcount<br/>Best performance<br/>Readers never block"]
    Q4 -- No --> RWSEM["rw_semaphore<br/>Writer priority"]
    Q3 -- No --> Q5{"Need to sleep?"}
    Q5 -- Yes --> RWSEM
    Q5 -- No --> RWLOCK["rwlock_t<br/>Spinning, simple"]
    
    style SEQLOCK fill:#2ecc71,stroke:#27ae60,color:#fff
    style RWSEM fill:#3498db,stroke:#2980b9,color:#fff
    style RWLOCK fill:#f39c12,stroke:#e67e22,color:#fff
    style MUTEX fill:#9b59b6,stroke:#8e44ad,color:#fff
```

| Feature | rwlock_t | rw_semaphore | seqlock_t |
|---------|----------|--------------|-----------|
| Wait behavior | Spin | Sleep | Reader: retry, Writer: spin |
| Reader concurrency | Yes | Yes | Yes (optimistic) |
| Writer starvation risk | HIGH | Low (writer pref) | None |
| Reader starvation risk | No | Yes (writers block new readers) | No |
| Reader blocks writer? | Yes | Yes | NO |
| Writer blocks reader? | Yes | Yes | NO (reader retries) |
| Usable in IRQ? | Yes | No | Reader: Yes, Writer: depends |
| Pointers in data? | Yes | Yes | CAREFUL (may read torn pointer) |
| Best use case | Moderate R:W ratio | Long critical sections | Extreme read-heavy (jiffies) |

---

## 10. SeqLock Limitation — Pointer Data

```c
/* DANGER: seqlock with pointers */
struct data {
    char *name;    /* pointer */
    int value;
};

/* Reader reads torn pointer (half old, half new) →
 * dereferences garbage → CRASH
 * 
 * SeqLock readers re-read data, but if you USE a pointer
 * between read_seqbegin and read_seqretry, you may
 * dereference a garbage half-updated pointer.
 * 
 * RULE: seqlock data should contain ONLY scalar values
 *       (integers, timestamps, counters).
 *       For pointer-based data → use RCU instead. */
```

---

## 11. Deep Q&A

### Q1: When would you choose rwlock_t over rw_semaphore?

**A:** Use `rwlock_t` when: (1) you need RW locking in IRQ/softirq context (rwsem cannot sleep there), (2) critical section is very short and spinning is acceptable, (3) writer starvation is not a concern. Use `rw_semaphore` when: (1) critical section may sleep, (2) writer starvation must be prevented, (3) process context only.

### Q2: Why can't seqlock protect pointer-based data?

**A:** Seqlock readers read the data, then check if a write happened. If the data contains a pointer and a writer changes it mid-read, the reader gets a partially-updated pointer. It might dereference this garbage pointer BEFORE `read_seqretry()` detects the inconsistency. With scalar data, a torn read just means a wrong number — detected and retried. With a pointer, a torn read causes an immediate crash or memory corruption.

### Q3: How does rw_semaphore prevent writer starvation?

**A:** When a writer is waiting, new readers are blocked from entering. The implementation queues all waiters (readers and writers) in FIFO order. When the current readers finish, the waiting writer goes first, then subsequent queued readers can enter. This is called "writer preference" or "fair" scheduling.

### Q4: Can you convert a write lock to a read lock atomically?

**A:** Yes, with `rw_semaphore`: `downgrade_write(&rwsem)` converts a write lock to a read lock without releasing and reacquiring. This is useful when you modify data and then want to continue reading while letting other readers in. `rwlock_t` does NOT have this feature.

---

[← Previous: 05 — Semaphores](05_Semaphores.md) | [Next: 07 — RCU →](07_RCU.md)
