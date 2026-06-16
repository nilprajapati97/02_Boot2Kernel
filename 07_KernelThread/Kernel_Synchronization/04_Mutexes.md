# 04 — Mutexes in the Linux Kernel

> **Scope**: struct mutex internals, adaptive spinning, wait/wake mechanism, mutex vs spinlock, mutex debugging, and real-world usage patterns.

---

## 1. What is a Mutex?

A **mutex** (mutual exclusion) is a **sleeping lock**. When a thread cannot acquire the mutex, it is put to sleep and added to a wait queue, freeing the CPU for other work.

```c
#include <linux/mutex.h>

/* Static initialization */
DEFINE_MUTEX(my_mutex);

/* Dynamic initialization */
struct mutex my_mutex;
mutex_init(&my_mutex);

/* Usage */
mutex_lock(&my_mutex);
/* ... critical section — CAN SLEEP here ... */
mutex_unlock(&my_mutex);
```

---

## 2. Mutex vs Spinlock Decision

```mermaid
flowchart TD
    START["Need to protect shared data?"]
    
    START --> Q1{"In interrupt context<br/>or atomic context?"}
    Q1 -- Yes --> SPIN["Use spinlock<br/>Cannot sleep in IRQ context"]
    Q1 -- No --> Q2{"Critical section long?<br/>May call sleeping functions?"}
    Q2 -- Yes --> MUTEX["Use mutex<br/>Sleep while waiting"]
    Q2 -- No --> Q3{"Very short section,<br/>high contention?"}
    Q3 -- Yes --> SPIN
    Q3 -- No --> MUTEX
    
    style SPIN fill:#e74c3c,stroke:#c0392b,color:#fff
    style MUTEX fill:#2ecc71,stroke:#27ae60,color:#fff
```

| Feature | Spinlock | Mutex |
|---------|----------|-------|
| Wait behavior | Busy-wait (spin) | Sleep (schedule out) |
| Context | Process + IRQ/softirq | Process context ONLY |
| Can sleep while held? | NO | YES |
| Overhead (uncontended) | Very low | Low |
| Overhead (contended) | Burns CPU | Context switch cost |
| Owner tracking | No | Yes |
| Recursive locking | Deadlock | Deadlock (use re-entrant variant) |
| Best for | Very short critical sections | Longer critical sections |

---

## 3. Mutex Internal Structure

```c
struct mutex {
    atomic_long_t owner;         /* Owner task + flags */
    raw_spinlock_t wait_lock;    /* Protects wait_list */
    struct list_head wait_list;  /* Sleeping waiters */
#ifdef CONFIG_DEBUG_MUTEXES
    struct task_struct *owner_task;
    const char *name;
    struct lock_class_key *key;
#endif
};

/* owner field encodes:
 * bits [2..N] = pointer to owning task_struct
 * bit 0 = MUTEX_FLAG_WAITERS (waiters exist)
 * bit 1 = MUTEX_FLAG_HANDOFF (handoff to first waiter)
 * bit 2 = MUTEX_FLAG_PICKUP (waiter should pick up) */
```

---

## 4. Mutex Lock/Unlock Flow — Three Paths

```mermaid
flowchart TD
    subgraph FastPath["Fast Path - Uncontended"]
        F1["mutex_lock called"]
        F2["atomic_try_cmpxchg owner<br/>0 to current task"]
        F3["Success: Lock acquired<br/>No spinlock, no waitqueue"]
        F1 --> F2 --> F3
    end
    
    subgraph MidPath["Mid Path - Adaptive Spinning"]
        M1["Fast path failed lock is held"]
        M2["Is owner running on another CPU?"]
        M3["YES: Spin in optimistic loop<br/>mutex_optimistic_spin"]
        M4["Owner releases lock while spinning"]
        M5["Acquire lock without sleeping"]
        M1 --> M2 --> M3 --> M4 --> M5
    end
    
    subgraph SlowPath["Slow Path - Sleep"]
        S1["Spinning failed or owner not running"]
        S2["Acquire wait_lock spinlock"]
        S3["Add to wait_list FIFO"]
        S4["Set MUTEX_FLAG_WAITERS"]
        S5["Release wait_lock"]
        S6["schedule - go to sleep"]
        S7["Woken up by unlock"]
        S8["Try to acquire lock again"]
        S1 --> S2 --> S3 --> S4 --> S5 --> S6 --> S7 --> S8
    end
    
    style F3 fill:#2ecc71,stroke:#27ae60,color:#fff
    style M5 fill:#3498db,stroke:#2980b9,color:#fff
    style S6 fill:#e74c3c,stroke:#c0392b,color:#fff
```

### Adaptive Spinning Sequence:

```mermaid
sequenceDiagram
    participant A as Task A - Lock Holder
    participant B as Task B - Contender
    participant S as Scheduler

    B->>B: mutex_lock - fast path fails
    Note over B: Check: Is Task A running on a CPU?

    loop Optimistic Spin
        B->>B: Check owner still running
        B->>B: cpu_relax - spin briefly
        Note over B: No context switch<br/>No waitqueue insertion
    end
    
    A->>A: mutex_unlock - owner = 0
    B->>B: Try cmpxchg - SUCCESS
    Note over B: Acquired lock via spinning<br/>Never slept, never touched waitqueue

    Note over A,B: KEY INSIGHT: spinning avoids<br/>two context switches save + restore<br/>which costs 2-10us each
```

---

## 5. Mutex Unlock Path

```c
void mutex_unlock(struct mutex *lock)
{
    /* Fast path: no waiters */
    if (atomic_long_try_cmpxchg_release(&lock->owner,
                                         current, 0))
        return;  /* Nobody waiting, just clear owner */
    
    /* Slow path: waiters exist */
    __mutex_unlock_slowpath(lock);
}

void __mutex_unlock_slowpath(struct mutex *lock)
{
    raw_spin_lock(&lock->wait_lock);
    
    if (!list_empty(&lock->wait_list)) {
        struct mutex_waiter *waiter;
        waiter = list_first_entry(&lock->wait_list,
                                  struct mutex_waiter, list);
        /* 
         * Set HANDOFF flag: tells the waiter 
         * to pick up the lock directly.
         * Prevents starvation from spinners.
         */
        wake_up_process(waiter->task);
    }
    
    raw_spin_unlock(&lock->wait_lock);
}
```

---

## 6. Mutex API Reference

```c
/* Blocking lock — can sleep */
void mutex_lock(struct mutex *lock);

/* Blocking with signal interruption */
int mutex_lock_interruptible(struct mutex *lock);
/* Returns 0 on success, -EINTR on signal */

/* Blocking with fatal signal only */
int mutex_lock_killable(struct mutex *lock);
/* Returns 0 on success, -EINTR on SIGKILL */

/* Non-blocking try */
int mutex_trylock(struct mutex *lock);
/* Returns 1 if acquired, 0 if not */

/* Unlock */
void mutex_unlock(struct mutex *lock);

/* Check if locked (for assertions, not synchronization!) */
bool mutex_is_locked(struct mutex *lock);
```

---

## 7. Mutex Rules and Constraints

```mermaid
flowchart TD
    subgraph Rules["Mutex Rules - Enforced by DEBUG_MUTEXES"]
        R1["Only the OWNER can unlock"]
        R2["Cannot lock in IRQ or softirq context"]
        R3["Cannot be used recursively"]
        R4["Must be unlocked before task exits"]
        R5["Cannot be used with spin_lock held"]
        R6["Lock and unlock must be in same context"]
    end
    
    subgraph Why["Why These Rules?"]
        W1["Owner tracked in owner field"]
        W2["Sleeping not allowed in atomic context"]
        W3["No recursion count, instant deadlock"]
        W4["No auto-release on task death"]
        W5["spinlock disables preempt, mutex may sleep"]
        W6["Ensures balanced lock/unlock pairing"]
    end
    
    R1 --- W1
    R2 --- W2
    R3 --- W3
    R4 --- W4
    R5 --- W5
    R6 --- W6

    style R1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style R2 fill:#e74c3c,stroke:#c0392b,color:#fff
    style R3 fill:#e74c3c,stroke:#c0392b,color:#fff
    style R4 fill:#e74c3c,stroke:#c0392b,color:#fff
    style R5 fill:#e74c3c,stroke:#c0392b,color:#fff
    style R6 fill:#e74c3c,stroke:#c0392b,color:#fff
```

---

## 8. Real-World: Device Driver Mutex Usage

```c
struct my_device {
    struct mutex config_lock;
    struct i2c_client *client;
    u8 config_regs[16];
};

int my_dev_read_config(struct my_device *dev, u8 reg, u8 *val)
{
    int ret;
    
    mutex_lock(&dev->config_lock);
    
    /* I2C transaction can sleep! Perfect for mutex. */
    ret = i2c_smbus_read_byte_data(dev->client, reg);
    if (ret < 0)
        goto out;
    
    *val = ret;
    dev->config_regs[reg] = ret;
    ret = 0;
    
out:
    mutex_unlock(&dev->config_lock);
    return ret;
}

/* Userspace access with interruptible variant */
ssize_t my_dev_write(struct file *file, const char __user *buf,
                     size_t count, loff_t *ppos)
{
    struct my_device *dev = file->private_data;
    int ret;
    
    /* Interruptible: user can Ctrl+C */
    ret = mutex_lock_interruptible(&dev->config_lock);
    if (ret)
        return -ERESTARTSYS;
    
    /* copy_from_user can sleep — fine under mutex */
    if (copy_from_user(dev->config_regs, buf, count)) {
        ret = -EFAULT;
        goto out;
    }
    
    ret = count;
out:
    mutex_unlock(&dev->config_lock);
    return ret;
}
```

---

## 9. Deep Q&A

### Q1: What is adaptive spinning and why does it matter?

**A:** When a mutex is contended, instead of immediately sleeping, the kernel checks if the owner is currently running on another CPU. If yes, it spins briefly (without disabling preemption, so it can be preempted itself). The rationale: if the owner is running, it will likely release the lock very soon, and spinning avoids two expensive context switches (sleep + wake). This was a major improvement — workloads with short critical sections saw 30-50% throughput gains.

### Q2: Why can't you use mutex in interrupt context?

**A:** `mutex_lock()` may call `schedule()` to sleep. Sleeping in interrupt context is forbidden because:
1. There's no `task_struct` to put to sleep (IRQ is not a schedulable entity)
2. Other interrupts might be blocked
3. The scheduler assumes process context
Using mutex in IRQ context triggers a BUG() with `might_sleep()` debugging.

### Q3: Explain the HANDOFF mechanism.

**A:** Without handoff, optimistic spinners can starve sleeping waiters. A spinner arriving just as the lock is released can grab it before the sleeping waiter is even woken up. The HANDOFF flag tells the unlock path: "Give the lock directly to the first sleeping waiter, don't let spinners steal it." This ensures fairness — a sleeping waiter gets priority after waiting long enough.

### Q4: When should you use mutex_lock_interruptible vs mutex_lock?

**A:** Use `mutex_lock_interruptible()` in system call paths where the user might Ctrl+C (send a signal). It returns -EINTR on signal, and you return -ERESTARTSYS to userspace. Use plain `mutex_lock()` in kernel-internal paths where you must complete regardless of signals. Use `mutex_lock_killable()` for operations that should only abort on fatal signals (SIGKILL).

---

[← Previous: 03 — Spinlocks](03_Spinlocks.md) | [Next: 05 — Semaphores →](05_Semaphores.md)
