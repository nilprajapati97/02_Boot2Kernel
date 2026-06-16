# 05 — Mutex

## 1. What is a Mutex?

A **mutex** (mutual exclusion lock) is a **sleeping**, **exclusive** lock. It's the preferred mechanism for protecting critical sections in **process context** where the section may need to sleep.

**Key properties:**
- Only the **owner** can unlock it
- Uncontended acquisition is very fast (no syscall)
- Contended: task is put to sleep (no CPU waste)
- Supports **adaptive spinning** (might spin briefly before sleeping)

---

## 2. Data Structure

```c
/* include/linux/mutex.h */
struct mutex {
    atomic_long_t   owner;   /* Task that owns + state bits */
    raw_spinlock_t  wait_lock;
    struct list_head wait_list;
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
    struct optimistic_spin_queue osq; /* Adaptive spin queue */
#endif
    /* ... lockdep fields ... */
};
```

---

## 3. API

```c
/* Static initialization */
DEFINE_MUTEX(my_mutex);

/* Dynamic initialization */
struct mutex my_mutex;
mutex_init(&my_mutex);

/* Lock — uninterruptible sleep (preferred) */
mutex_lock(&my_mutex);

/* Lock — interruptible (returns -EINTR on signal) */
mutex_lock_interruptible(&my_mutex);

/* Lock — killable (returns -EINTR only on fatal signal) */
mutex_lock_killable(&my_mutex);

/* Trylock — returns 1 if acquired, 0 if not */
mutex_trylock(&my_mutex);

/* Unlock — ONLY the owner can call this */
mutex_unlock(&my_mutex);

/* Debug: is it locked? */
mutex_is_locked(&my_mutex);
```

---

## 4. Mutex vs Spinlock

| | Mutex | Spinlock |
|-|-------|---------|
| Waiting behavior | Sleep (no CPU waste) | Spin (busy-wait) |
| Can hold and sleep? | Yes | No |
| Interrupt context? | No | Yes |
| Overhead (uncontended) | Low (atomic CAS) | Very low |
| Critical section length | Long | Short |
| Ownership | Yes (only owner can unlock) | No |

---

## 5. Adaptive Spinning (Mutex Optimization)

Before sleeping, a mutex waiter will **spin briefly** if the owner is running on another CPU:

```mermaid
flowchart TD
    A[Try mutex_lock] --> B{Uncontended?}
    B -- Yes --> C[Acquire: fast path]
    B -- No --> D{Owner running\non another CPU?}
    D -- Yes --> E[Spin briefly\n(optimistic spin)]
    E --> F{Lock freed?}
    F -- Yes --> C
    F -- No, owner slept --> G[Go to sleep\n(add to wait_list)]
    D -- No --> G
    G --> H[Wait for wake_up]
    H --> C
```

This avoids the expensive sleep/wake cycle for short-held mutexes.

---

## 6. Usage Example

```c
#include <linux/mutex.h>

struct my_device {
    struct mutex    lock;
    int             data[1024];
    bool            initialized;
};

static struct my_device dev;

static int __init my_device_init(void)
{
    mutex_init(&dev.lock);
    return 0;
}

/* Read operation — can sleep */
int read_device(int idx, int *out)
{
    int ret;
    
    if (mutex_lock_interruptible(&dev.lock))
        return -ERESTARTSYS;
    
    if (!dev.initialized) {
        ret = -ENODEV;
        goto out;
    }
    
    *out = dev.data[idx];
    ret = 0;
out:
    mutex_unlock(&dev.lock);
    return ret;
}

/* Write operation */
int write_device(int idx, int val)
{
    if (mutex_lock_killable(&dev.lock))
        return -EINTR;
    
    dev.data[idx] = val;
    
    mutex_unlock(&dev.lock);
    return 0;
}
```

---

## 7. Common Pitfalls

```c
/* WRONG: Using mutex in interrupt context */
static irqreturn_t my_isr(int irq, void *dev)
{
    mutex_lock(&lock);  /* BUG: Can sleep! IRQ context cannot sleep */
    /* ... */
    mutex_unlock(&lock);
    return IRQ_HANDLED;
}
/* FIX: Use spin_lock_irqsave() instead */

/* WRONG: Non-owner trying to unlock */
void thread_a(void) { mutex_lock(&lock); }
void thread_b(void) { mutex_unlock(&lock); }  /* BUG: not the owner */

/* WRONG: Recursive mutex (deadlock unless using recursive mutex type) */
mutex_lock(&lock);
some_func();  /* some_func() also calls mutex_lock(&lock) → DEADLOCK */
```

---

## 8. Source Files

| File | Description |
|------|-------------|
| `include/linux/mutex.h` | API |
| `kernel/locking/mutex.c` | Implementation with adaptive spin |

---

## 9. Related Concepts
- [02_Spin_Locks.md](./02_Spin_Locks.md) — For IRQ context / short sections
- [04_Semaphores.md](./04_Semaphores.md) — Counting / non-ownership sleeping lock
- [06_Completion_Variables.md](./06_Completion_Variables.md) — One-shot event signaling
