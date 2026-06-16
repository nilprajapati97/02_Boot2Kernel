# 01 — Atomic Operations

## 1. What are Atomic Operations?

**Atomic operations** complete in a **single, uninterruptible step** — no other CPU can observe a partial result. They are the foundation of all higher-level synchronization.

**Key uses:**
- Reference counters (e.g., `refcount_t`, `kref`)
- Statistics/counters
- Flag bits
- Lock-free algorithms

---

## 2. atomic_t — Integer Operations

```c
/* include/linux/atomic.h */
typedef struct { int counter; } atomic_t;
typedef struct { long counter; } atomic64_t;

/* Initialize */
atomic_t v = ATOMIC_INIT(0);
atomic64_t v64 = ATOMIC64_INIT(0);

/* Read */
int val = atomic_read(&v);             /* Guaranteed atomic load */

/* Write */
atomic_set(&v, 5);                     /* Atomic store */

/* Arithmetic */
atomic_inc(&v);                        /* v++ */
atomic_dec(&v);                        /* v-- */
atomic_add(n, &v);                     /* v += n */
atomic_sub(n, &v);                     /* v -= n */

/* Return new value */
int new = atomic_inc_return(&v);       /* ++v, return new */
int new = atomic_dec_return(&v);       /* --v, return new */
int old = atomic_fetch_add(n, &v);     /* v += n, return old */

/* Test-and-set patterns */
if (atomic_dec_and_test(&v))           /* Decrement; true if now 0 */
    /* Last reference — free resource */;

if (atomic_inc_not_zero(&v))           /* Increment only if not 0 */
    /* Got reference */;
```

---

## 3. Reference Counting Pattern

```c
/* Common kernel pattern: refcount in object */
struct my_object {
    atomic_t    refcnt;
    /* ... */
};

struct my_object *my_obj_get(struct my_object *obj)
{
    atomic_inc(&obj->refcnt);
    return obj;
}

void my_obj_put(struct my_object *obj)
{
    if (atomic_dec_and_test(&obj->refcnt))
        kfree(obj);  /* Last reference — free */
}
```

> **Modern kernel uses `refcount_t`** (wraps atomic_t with overflow protection):
```c
refcount_t ref = REFCOUNT_INIT(1);
refcount_inc(&ref);
if (refcount_dec_and_test(&ref)) kfree(obj);
```

---

## 4. Bit Operations

```c
/* include/linux/bitops.h */
/* These are atomic on x86: */
set_bit(nr, addr);        /* Set bit nr in *addr */
clear_bit(nr, addr);      /* Clear bit nr */
change_bit(nr, addr);     /* Toggle bit nr */
test_bit(nr, addr);       /* Read bit (non-atomic) */
test_and_set_bit(nr, addr);    /* Returns old value, sets bit */
test_and_clear_bit(nr, addr);  /* Returns old value, clears bit */

/* Non-atomic versions (faster for local/unshared data) */
__set_bit(nr, addr);
__clear_bit(nr, addr);
```

**Usage in kernel:**
```c
/* task_struct flags */
set_bit(TIF_NEED_RESCHED, &tsk->thread_info.flags);
if (test_and_clear_bit(TIF_SIGPENDING, &tsk->thread_info.flags))
    do_signal();
```

---

## 5. x86 Implementation

```c
/* arch/x86/include/asm/atomic.h */
static __always_inline void atomic_inc(atomic_t *v)
{
    asm volatile(LOCK_PREFIX "incl %0"
                 : "+m" (v->counter));
}

/* LOCK_PREFIX = "lock " on SMP, empty on UP */
/* "lock incl" — bus lock prevents other CPUs from interrupting */
```

---

## 6. Memory Barriers

Atomic ops on x86 include implicit memory ordering, but some architectures need explicit barriers:

```c
/* Full memory barrier */
smp_mb();       /* After this, all previous reads/writes visible to all CPUs */

/* Read/write barriers */
smp_rmb();      /* All previous reads complete before next read */
smp_wmb();      /* All previous writes complete before next write */

/* Compiler barrier (prevents reordering by compiler, not CPU) */
barrier();

/* Ordered atomic ops */
smp_store_mb(var, val);     /* store + smp_mb() */
smp_load_acquire(&var);     /* load with acquire semantics */
smp_store_release(&var, val); /* store with release semantics */
```

---

## 7. WRITE_ONCE / READ_ONCE

For non-atomic shared variables, use these to prevent compiler from caching/reordering:

```c
/* Without: compiler may optimize away the loop (read once, cache) */
while (shared_flag) { }  /* BUG: compiler may make this infinite loop */

/* With READ_ONCE: compiler reads flag every iteration */
while (READ_ONCE(shared_flag)) { }

/* For writes that must be visible to other CPUs: */
WRITE_ONCE(shared_flag, 0);
```

---

## 8. Source Files

| File | Description |
|------|-------------|
| `include/linux/atomic.h` | atomic_t, atomic_inc, etc. |
| `include/linux/refcount.h` | refcount_t (overflow-safe) |
| `include/linux/bitops.h` | set_bit, test_bit |
| `arch/x86/include/asm/atomic.h` | x86-specific implementation |
| `arch/x86/include/asm/barrier.h` | Memory barrier implementation |

---

## 9. Related Concepts
- [02_Spin_Locks.md](./02_Spin_Locks.md) — For multi-instruction critical sections
- [09_Memory_Ordering.md](./09_Memory_Ordering.md) — Memory barriers in depth
- [../08_Intro_To_Kernel_Synchronization/01_Critical_Regions.md](../08_Intro_To_Kernel_Synchronization/01_Critical_Regions.md)
