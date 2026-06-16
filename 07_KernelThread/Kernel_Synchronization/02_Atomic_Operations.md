# 02 — Atomic Operations in the Linux Kernel

> **Scope**: atomic_t, atomic64_t, bitwise atomics, atomic operations on ARM and x86, memory ordering with atomics, and when to use them vs locks.

---

## 1. What are Atomic Operations?

An atomic operation is **indivisible** — it completes entirely or not at all. No other CPU or interrupt can observe an intermediate state.

```mermaid
flowchart LR
    subgraph NonAtomic["Non-Atomic i++"]
        N1["LOAD i from RAM"] --> N2["ADD 1 in register"] --> N3["STORE i to RAM"]
        N4["Other CPU can<br/>read/write i HERE"] -.-> N2
    end
    
    subgraph Atomic["atomic_inc"]
        A1["LOCK; INC [i]<br/>Single indivisible operation<br/>Bus locked or cache-line exclusive"]
    end

    style N4 fill:#e74c3c,stroke:#c0392b,color:#fff
    style A1 fill:#2ecc71,stroke:#27ae60,color:#fff
```

---

## 2. Kernel Atomic Types

```c
/* include/linux/types.h */
typedef struct {
    int counter;
} atomic_t;

typedef struct {
    s64 counter;
} atomic64_t;

/* The struct wrapping prevents accidental direct access.
 * Forces use of atomic_*() API. */
```

---

## 3. Core Atomic API

```mermaid
flowchart TD
    subgraph Init["Initialization"]
        I1["ATOMIC_INIT(val)<br/>atomic_t v = ATOMIC_INIT(5)"]
        I2["atomic_set(v, val)<br/>atomic_set(&v, 10)"]
    end
    
    subgraph Read["Read"]
        R1["atomic_read(v)<br/>Returns current value<br/>Simple READ, no barrier"]
    end
    
    subgraph Modify["Read-Modify-Write"]
        M1["atomic_inc(v)<br/>v++"]
        M2["atomic_dec(v)<br/>v--"]
        M3["atomic_add(i, v)<br/>v += i"]
        M4["atomic_sub(i, v)<br/>v -= i"]
    end
    
    subgraph TestReturn["Test and Return"]
        T1["atomic_inc_return(v)<br/>return ++v"]
        T2["atomic_dec_return(v)<br/>return --v"]
        T3["atomic_dec_and_test(v)<br/>return --v == 0"]
        T4["atomic_add_return(i, v)<br/>return v += i"]
    end
    
    subgraph CAS["Compare-and-Swap"]
        C1["atomic_cmpxchg(v, old, new)<br/>if v == old then v = new<br/>returns old value"]
        C2["atomic_try_cmpxchg(v, old, new)<br/>returns true if swapped"]
        C3["atomic_xchg(v, new)<br/>swap v with new<br/>returns old value"]
    end

    style M1 fill:#3498db,stroke:#2980b9,color:#fff
    style T3 fill:#f39c12,stroke:#e67e22,color:#fff
    style C1 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 4. How Atomics Work on x86 vs ARM

### 4.1 x86: LOCK Prefix

```c
/* x86 atomic_inc implementation (simplified): */
static inline void atomic_inc(atomic_t *v)
{
    asm volatile("lock; incl %0"
                 : "+m" (v->counter));
}

/*
 * The LOCK prefix:
 * 1. Acquires exclusive ownership of the cache line (MESI E/M state)
 * 2. Prevents other CPUs from accessing that cache line
 * 3. Performs the increment
 * 4. Releases cache line
 * 
 * On modern CPUs: NOT a bus lock (slow).
 * Instead: cache-line lock via MESI protocol.
 * Cost: ~10-20 cycles (vs ~1 cycle for non-atomic)
 */
```

### 4.2 ARM: LDREX/STREX (LL/SC)

```c
/* ARM atomic_inc implementation (simplified ARMv7): */
static inline void atomic_inc(atomic_t *v)
{
    unsigned long tmp;
    int result;

    asm volatile(
        "1: ldrex   %0, [%3]       \n"  /* Load-Exclusive: read v->counter */
        "   add     %0, %0, #1     \n"  /* Increment */
        "   strex   %1, %0, [%3]   \n"  /* Store-Exclusive: try write back */
        "   teq     %1, #0         \n"  /* Did STREX succeed? */
        "   bne     1b             \n"  /* No? Retry from LDREX */
        : "=&r" (result), "=&r" (tmp), "+Qo" (v->counter)
        : "r" (&v->counter)
        : "cc");
}

/*
 * LL/SC (Load-Link / Store-Conditional):
 * - LDREX marks the cache line as "exclusive monitor"
 * - STREX succeeds ONLY if no other CPU touched that line
 * - If another CPU wrote to it, STREX fails → retry
 * - No bus lock needed! More scalable than x86 LOCK.
 * 
 * ARMv8.1+: Uses LSE (Large System Extensions)
 *   LDADD, STADD — single-instruction atomics
 *   Much faster than LL/SC loop
 */
```

```mermaid
sequenceDiagram
    participant CPU0 as CPU 0
    participant CACHE as Cache Line
    participant CPU1 as CPU 1

    Note over CPU0,CPU1: Both do atomic_inc on same variable

    CPU0->>CACHE: LDREX - load exclusive, val=5
    CPU1->>CACHE: LDREX - load exclusive, val=5
    
    CPU0->>CPU0: ADD 1 = 6
    CPU1->>CPU1: ADD 1 = 6
    
    CPU0->>CACHE: STREX - store exclusive val=6
    Note over CPU0: SUCCESS - first to store
    
    CPU1->>CACHE: STREX - store exclusive val=6
    Note over CPU1: FAIL - exclusive monitor cleared by CPU0
    
    CPU1->>CACHE: LDREX - retry, load exclusive, val=6
    CPU1->>CPU1: ADD 1 = 7
    CPU1->>CACHE: STREX - store exclusive val=7
    Note over CPU1: SUCCESS

    Note over CACHE: Final value = 7 CORRECT
```

---

## 5. Bitwise Atomic Operations

```c
/* Operate on individual bits atomically */

/* Set bit N in the word pointed to by addr */
void set_bit(int nr, volatile unsigned long *addr);

/* Clear bit N */
void clear_bit(int nr, volatile unsigned long *addr);

/* Toggle bit N */
void change_bit(int nr, volatile unsigned long *addr);

/* Test and set: returns OLD value of bit N */
int test_and_set_bit(int nr, volatile unsigned long *addr);

/* Test and clear */
int test_and_clear_bit(int nr, volatile unsigned long *addr);

/* Non-atomic versions (for use under lock): */
void __set_bit(int nr, volatile unsigned long *addr);
void __clear_bit(int nr, volatile unsigned long *addr);

/* Usage example: device status flags */
#define DEV_RUNNING    0
#define DEV_SUSPENDED  1
#define DEV_ERROR      2

unsigned long dev_flags;

/* ISR sets error flag atomically */
set_bit(DEV_ERROR, &dev_flags);

/* Process context checks and clears */
if (test_and_clear_bit(DEV_ERROR, &dev_flags)) {
    handle_error();
}
```

---

## 6. Memory Ordering with Atomics

```mermaid
flowchart TD
    subgraph Ordering["Memory Ordering Variants"]
        RELAXED["atomic_inc<br/>atomic_dec<br/>atomic_add<br/>NO ordering guarantee<br/>Just atomicity"]
        
        ACQUIRE["atomic_read_acquire<br/>smp_load_acquire<br/>All reads/writes AFTER<br/>this are visible after it"]
        
        RELEASE["atomic_set_release<br/>smp_store_release<br/>All reads/writes BEFORE<br/>this are visible before it"]
        
        FULL["atomic_add_return<br/>atomic_cmpxchg<br/>atomic_xchg<br/>FULL memory barrier<br/>Both acquire + release"]
    end

    style RELAXED fill:#2ecc71,stroke:#27ae60,color:#fff
    style ACQUIRE fill:#3498db,stroke:#2980b9,color:#fff
    style RELEASE fill:#f39c12,stroke:#e67e22,color:#fff
    style FULL fill:#e74c3c,stroke:#c0392b,color:#fff
```

```c
/* Relaxed: just atomic, no ordering */
atomic_inc(&counter);  /* other CPUs may see this out of order */

/* With ordering: */
atomic_inc_return(&counter);  /* implies full barrier */

/* Explicit barrier variants: */
smp_store_release(&flag, 1);   /* everything before is visible first */
val = smp_load_acquire(&flag); /* everything after sees the store */
```

---

## 7. Reference Counting with Atomics

```c
/* The most common use of atomics: reference counting */
struct kref {
    refcount_t refcount;  /* wraps atomic_t with overflow protection */
};

/* refcount_t is like atomic_t BUT:
 * - WARNS on increment from 0 (use-after-free)
 * - SATURATES instead of wrapping on overflow
 * - Detects decrement below 0
 */

struct my_object {
    struct kref ref;
    char *name;
};

struct my_object *obj_get(struct my_object *obj)
{
    kref_get(&obj->ref);  /* atomic_inc_not_zero internally */
    return obj;
}

void obj_release(struct kref *kref)
{
    struct my_object *obj = container_of(kref, struct my_object, ref);
    kfree(obj->name);
    kfree(obj);
}

void obj_put(struct my_object *obj)
{
    kref_put(&obj->ref, obj_release);
    /* atomic_dec_and_test: if refcount hits 0, call obj_release */
}
```

```mermaid
sequenceDiagram
    participant A as Thread A
    participant OBJ as Object refcount
    participant B as Thread B

    Note over OBJ: refcount = 1

    A->>OBJ: kref_get - refcount = 2
    B->>OBJ: kref_get - refcount = 3

    B->>OBJ: kref_put - refcount = 2
    A->>OBJ: kref_put - refcount = 1

    Note over A: Last reference holder
    A->>OBJ: kref_put - refcount = 0
    Note over OBJ: RELEASE called<br/>Object freed
```

---

## 8. When to Use Atomics vs Locks

| Scenario | Use | Why |
|----------|-----|-----|
| Simple counter (stats) | `atomic_t` | No relationship with other data |
| Reference count | `refcount_t` | Overflow protection built in |
| Single flag check | `atomic_t` or bit ops | Lightweight, no contention |
| Counter + linked list update | Spinlock/mutex | Multiple operations must be atomic together |
| Read-modify of a struct | Lock | Cannot atomically update multiple fields |
| Increment from interrupt + process | `atomic_t` | No need to disable IRQ for single op |

```mermaid
flowchart TD
    Q1{"How many variables<br/>need protection?"} -->|"Just one integer<br/>or one flag"| A["Use atomic_t<br/>or bit operations"]
    Q1 -->|"Multiple related<br/>variables"| L["Use a lock<br/>spinlock or mutex"]
    
    A --> Q2{"Need ordering<br/>with other operations?"}
    Q2 -->|"No"| A2["atomic_inc<br/>atomic_dec<br/>relaxed"]
    Q2 -->|"Yes"| A3["atomic_inc_return<br/>or explicit barriers"]
    
    L --> Q3{"Can you sleep?"}
    Q3 -->|"Yes"| MUTEX["mutex"]
    Q3 -->|"No - IRQ context"| SPIN["spinlock"]

    style A fill:#2ecc71,stroke:#27ae60,color:#fff
    style MUTEX fill:#3498db,stroke:#2980b9,color:#fff
    style SPIN fill:#f39c12,stroke:#e67e22,color:#fff
```

---

## 9. Deep Q&A

### Q1: Why does Linux have both atomic_t and refcount_t?

**A:** `atomic_t` has no overflow/underflow protection — incrementing from `INT_MAX` wraps to negative, decrementing from 0 wraps to `INT_MAX`. Attackers can exploit this. `refcount_t` saturates at `REFCOUNT_SATURATED` instead of wrapping, and warns on inc-from-zero. Since 4.11, all reference counting should use `refcount_t`.

### Q2: Is atomic_read() truly atomic?

**A:** On most architectures, a naturally-aligned word-sized read is already atomic at the hardware level. `atomic_read()` is essentially `READ_ONCE(v->counter)` — it prevents compiler optimizations (reordering, caching in register) but doesn't use any special instruction. It does NOT imply a memory barrier.

### Q3: Can atomic operations starve on ARM with LL/SC?

**A:** Theoretically yes — if other CPUs constantly invalidate the cache line between LDREX and STREX, the loop retries indefinitely. In practice, ARM's exclusive monitor has fairness mechanisms, and the window is tiny. ARMv8.1 LSE instructions (LDADD, STADD) eliminate this entirely.

### Q4: What is the cost of an atomic operation?

**A:** On x86 with LOCK prefix: ~10-20 cycles if cache line is local, ~40-100 cycles if cache line must be fetched from another CPU's cache (MESI invalidation). Compare: simple register increment = 1 cycle. Atomics are cheap vs locks but still 10-100x costlier than non-atomic ops.

---

[← Previous: 01 — Race Conditions](01_Race_Conditions_and_Critical_Sections.md) | [Next: 03 — Spinlocks →](03_Spinlocks.md)
