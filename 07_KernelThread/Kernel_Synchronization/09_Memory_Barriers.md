# 09 — Memory Barriers

> **Scope**: Why memory barriers exist, compiler barriers, CPU barriers (smp_mb, smp_wmb, smp_rmb), READ_ONCE/WRITE_ONCE, acquire/release semantics, barrier pairing, and architecture-specific behavior (x86 TSO vs ARM weak ordering).

---

## 1. Why Memory Barriers?

Both the **compiler** and the **CPU** can reorder memory accesses for performance. On a single CPU this is invisible, but on SMP systems it creates bugs.

```mermaid
flowchart TD
    subgraph Reorder["Who Reorders?"]
        C["Compiler Reordering<br/>gcc/clang moves loads/stores<br/>for optimization"]
        CPU["CPU Reordering<br/>Out-of-order execution,<br/>store buffers, write combining"]
    end
    
    subgraph Problem["The Problem on SMP"]
        P1["CPU 0 writes: data=42, flag=1"]
        P2["CPU 1 reads: if flag==1, use data"]
        P3["Without barriers: CPU 1 may see<br/>flag=1 but data=0<br/>because writes were reordered"]
    end
    
    subgraph Solution["Solution: Memory Barriers"]
        S1["Force ordering of memory operations"]
        S2["Compiler barriers: prevent compiler reorder"]
        S3["CPU barriers: prevent CPU reorder"]
    end
    
    Reorder --> Problem --> Solution

    style P3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style S1 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 2. Classic Problem: Flag-Based Communication

```mermaid
sequenceDiagram
    participant CPU0 as CPU 0 - Producer
    participant MEM as Shared Memory
    participant CPU1 as CPU 1 - Consumer

    Note over CPU0: Write data THEN set flag
    CPU0->>MEM: data = 42
    CPU0->>MEM: flag = 1

    Note over CPU0,CPU1: WITHOUT barrier:<br/>CPU store buffer may<br/>release flag=1 before data=42

    CPU1->>MEM: Read flag - sees 1
    CPU1->>MEM: Read data - sees 0 STALE
    Note over CPU1: BUG: flag says ready<br/>but data is not there yet

    Note over CPU0,CPU1: WITH smp_wmb between writes<br/>and smp_rmb between reads:<br/>Correct ordering guaranteed
```

```c
/* BROKEN: no barriers */
/* CPU 0 */                 /* CPU 1 */
data = 42;                  if (flag == 1)
flag = 1;                       use(data); /* may see 0! */

/* CORRECT: with barriers */
/* CPU 0 */                 /* CPU 1 */
data = 42;                  if (READ_ONCE(flag) == 1) {
smp_wmb();  /* write       smp_rmb();  /* read
               barrier */                  barrier */
WRITE_ONCE(flag, 1);           use(data); /* guaranteed 42 */
                            }
```

---

## 3. Barrier Types in Linux

```mermaid
flowchart TD
    subgraph Compiler["Compiler Barriers"]
        CB1["barrier()<br/>Prevents compiler reordering<br/>across this point"]
        CB2["READ_ONCE(x)<br/>Forces actual memory read<br/>Prevents caching in register"]
        CB3["WRITE_ONCE(x, v)<br/>Forces actual memory write<br/>Prevents merging/elision"]
    end
    
    subgraph CPU["CPU Memory Barriers"]
        MB["smp_mb()<br/>Full barrier<br/>All prior reads/writes complete<br/>before any subsequent read/write"]
        WMB["smp_wmb()<br/>Write barrier<br/>All prior writes complete<br/>before subsequent writes"]
        RMB["smp_rmb()<br/>Read barrier<br/>All prior reads complete<br/>before subsequent reads"]
    end
    
    subgraph AcqRel["Acquire / Release"]
        ACQ["smp_load_acquire(p)<br/>Read + acquire barrier<br/>Subsequent reads/writes cannot<br/>move before this load"]
        REL["smp_store_release(p, v)<br/>Write + release barrier<br/>Prior reads/writes cannot<br/>move after this store"]
    end
    
    style MB fill:#e74c3c,stroke:#c0392b,color:#fff
    style WMB fill:#f39c12,stroke:#e67e22,color:#fff
    style RMB fill:#3498db,stroke:#2980b9,color:#fff
    style ACQ fill:#2ecc71,stroke:#27ae60,color:#fff
    style REL fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 4. Barrier Pairing Rules

**Every barrier on the writer side must be matched with a barrier on the reader side.**

```c
/* Write side                Read side */
WRITE_ONCE(a, 1);           r1 = READ_ONCE(b);
smp_wmb();        <----->   smp_rmb();
WRITE_ONCE(b, 1);           r2 = READ_ONCE(a);
/* If r1 == 1, then r2 is guaranteed == 1 */

/* Acquire/Release (preferred modern style) */
WRITE_ONCE(a, 1);           r1 = smp_load_acquire(&b);
smp_store_release(&b, 1);   r2 = READ_ONCE(a);
/* Same guarantee, cleaner API */
```

```mermaid
flowchart LR
    subgraph Writer["Writer CPU"]
        W1["WRITE a"]
        W2["smp_wmb or smp_store_release"]
        W3["WRITE b"]
        W1 --> W2 --> W3
    end
    
    subgraph Reader["Reader CPU"]
        R1["READ b"]
        R2["smp_rmb or smp_load_acquire"]
        R3["READ a"]
        R1 --> R2 --> R3
    end
    
    W2 -.->|"Paired barriers"| R2

    style W2 fill:#f39c12,stroke:#e67e22,color:#fff
    style R2 fill:#3498db,stroke:#2980b9,color:#fff
```

---

## 5. Architecture Differences

```mermaid
flowchart TD
    subgraph x86["x86 / x86_64 - TSO"]
        X1["Total Store Ordering"]
        X2["Reads NOT reordered with reads"]
        X3["Writes NOT reordered with writes"]
        X4["Reads NOT reordered before<br/>earlier writes to SAME location"]
        X5["smp_wmb = compiler barrier only"]
        X6["smp_rmb = compiler barrier only"]
        X7["smp_mb = MFENCE or LOCK instruction"]
        X8["ONLY reorder: later loads pass<br/>earlier stores to DIFFERENT addresses"]
    end
    
    subgraph ARM["ARM / ARM64 - Weak Ordering"]
        A1["Weakly ordered architecture"]
        A2["ALL reorderings are possible"]
        A3["smp_wmb = DMB ST"]
        A4["smp_rmb = DMB LD"]
        A5["smp_mb  = DMB ISH"]
        A6["Barriers are actual CPU instructions<br/>Real performance cost"]
    end
    
    style X5 fill:#2ecc71,stroke:#27ae60,color:#fff
    style X6 fill:#2ecc71,stroke:#27ae60,color:#fff
    style A6 fill:#e74c3c,stroke:#c0392b,color:#fff
```

| Reorder Type | x86 | ARM | PowerPC |
|-------------|-----|-----|---------|
| Load-Load | NO | YES | YES |
| Load-Store | NO | YES | YES |
| Store-Store | NO | YES | YES |
| Store-Load | **YES** | YES | YES |

---

## 6. READ_ONCE / WRITE_ONCE

```c
/* Problem: compiler can optimize away or merge accesses */
while (flag == 0) { }  
/* Compiler may load flag ONCE and loop forever
 * because it proves flag doesn't change in THIS function */

/* Solution: READ_ONCE forces a genuine memory load each time */
while (READ_ONCE(flag) == 0) { }

/* WRITE_ONCE prevents write tearing and elision */
WRITE_ONCE(flag, 1);
/* Prevents: compiler combining with other writes,
 * invented writes, partial writes on non-atomic types */
```

### When to use:

```mermaid
flowchart TD
    Q1{"Variable accessed by<br/>multiple CPUs or contexts?"}
    Q1 -- No --> PLAIN["Plain access OK"]
    Q1 -- Yes --> Q2{"Protected by a lock<br/>spinlock, mutex etc?"}
    Q2 -- Yes --> PLAIN2["Plain access OK<br/>Lock provides barriers"]
    Q2 -- No --> ONCE["Must use READ_ONCE<br/>and WRITE_ONCE"]
    
    style ONCE fill:#e74c3c,stroke:#c0392b,color:#fff
    style PLAIN fill:#2ecc71,stroke:#27ae60,color:#fff
    style PLAIN2 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 7. Acquire / Release Semantics

```c
/* Modern preferred pattern — replaces explicit barriers */

/* Release: all prior memory ops are visible before this store */
smp_store_release(&published, data_ptr);
/* Equivalent to: smp_wmb(); WRITE_ONCE(published, data_ptr); */

/* Acquire: all subsequent memory ops happen after this load */
p = smp_load_acquire(&published);
/* Equivalent to: p = READ_ONCE(published); smp_rmb(); */

/* KEY ADVANTAGE:
 * Acquire/release are one-directional barriers.
 * smp_mb() is two-directional (stronger, slower).
 * Acquire/release give the CPU more freedom to optimize
 * while still providing the guarantee you actually need. */
```

```mermaid
flowchart LR
    subgraph Before["Cannot move AFTER release"]
        B1["write X"]
        B2["write Y"]
    end
    
    REL["smp_store_release Z = 1"]
    
    subgraph After["CAN move freely here"]
        A1["write W"]
    end
    
    Before --> REL --> After
    
    subgraph After2["Cannot move BEFORE acquire"]
        C1["read X"]
        C2["read Y"]
    end
    
    ACQ["val = smp_load_acquire Z"]
    
    subgraph Before2["CAN move freely here"]
        D1["read W"]
    end
    
    Before2 --> ACQ --> After2

    style REL fill:#f39c12,stroke:#e67e22,color:#fff
    style ACQ fill:#3498db,stroke:#2980b9,color:#fff
```

---

## 8. Barriers Inside Lock Operations

```c
/* You DON'T need explicit barriers when using locks:
 * spin_lock/spin_unlock include barriers internally */

spin_lock(&lock);     /* includes ACQUIRE barrier */
/* Everything here is ordered after the lock */
shared_data = 42;
spin_unlock(&lock);   /* includes RELEASE barrier */
/* Everything above is ordered before the unlock */

/* mutex_lock/mutex_unlock: same implicit barriers */

/* atomic operations with _acquire/_release:
 * atomic_read_acquire(), atomic_set_release()
 * include the appropriate one-directional barrier */
```

---

## 9. Real Kernel Example: Ring Buffer

```c
/* Classic lock-free producer-consumer ring buffer */
struct ring {
    void *buffer[RING_SIZE];
    unsigned int head;  /* Producer writes here */
    unsigned int tail;  /* Consumer reads here */
};

/* Producer: */
void ring_produce(struct ring *r, void *item)
{
    unsigned int h = READ_ONCE(r->head);
    
    r->buffer[h % RING_SIZE] = item;
    
    /* Write barrier: ensure buffer write visible 
     * before head update */
    smp_store_release(&r->head, h + 1);
}

/* Consumer: */
void *ring_consume(struct ring *r)
{
    unsigned int t = r->tail;
    unsigned int h;
    void *item;
    
    /* Acquire: ensure we see producer's buffer write
     * if we see the head update */
    h = smp_load_acquire(&r->head);
    
    if (t == h)
        return NULL;  /* Empty */
    
    item = r->buffer[t % RING_SIZE];
    WRITE_ONCE(r->tail, t + 1);
    
    return item;
}
```

---

## 10. Deep Q&A

### Q1: Why is smp_mb needed on x86 when x86 has TSO?

**A:** TSO prevents load-load, store-store, and load-store reordering, but allows **store-load** reordering: a later load can pass an earlier store (because of the store buffer). `smp_mb()` on x86 uses MFENCE or a LOCK prefix to flush the store buffer and prevent this. This matters in algorithms like Dekker's mutual exclusion or seqlock readers.

### Q2: What is the difference between barrier() and smp_mb()?

**A:** `barrier()` is a compiler-only barrier: prevents the compiler from moving C statements across it, but the CPU can still reorder at runtime. `smp_mb()` is both compiler + CPU barrier: prevents both compiler and hardware reordering. On UP, `smp_mb()` degrades to just `barrier()` because there's only one CPU.

### Q3: Can READ_ONCE/WRITE_ONCE replace memory barriers?

**A:** No. READ_ONCE/WRITE_ONCE prevent the compiler from optimizing away or merging accesses, but they do NOT prevent CPU reordering. You need `smp_wmb()`, `smp_rmb()`, or acquire/release semantics to order operations across CPUs. Think of READ_ONCE/WRITE_ONCE as "compiler barriers for individual accesses" and smp_* as "CPU ordering guarantees."

### Q4: Why are acquire/release preferred over smp_wmb/smp_rmb?

**A:** Three reasons: (1) Intent: acquire/release express the pattern "publish data" clearly. (2) Performance: they are one-directional barriers, giving the CPU more optimization room than a full `smp_mb()`. (3) Correctness: they are associated with a specific variable access, so the reader-writer pairing is explicit and harder to get wrong. The kernel documentation recommends acquire/release for new code.

---

[← Previous: 08 — Completions](08_Completions.md) | [Next: 10 — Per-CPU Variables →](10_Per_CPU_Variables.md)
