# Day 14 — Sync Primitives: Spinlocks, Wait Queues, Semaphores

> **Goal**: Implement IRQ-save spinlocks (UP-correct, ready for ARMv8 LDXR/STXR on Day 28), wait queues with `wait_event` / `wake_up`, mutex (sleeping), semaphore, and completion.
>
> **Why today**: Day 15's `fork`, Day 17's syscall path, Day 20's VFS, and Day 22's virtio all need to sleep on conditions. Without wait queues, every blocking call would have to spin.

---

## 1. Background

### 1.1 Two flavors of locks
- **Spinlock**: never sleeps; held with IRQs disabled. Use when critical section is short and may run in IRQ context.
- **Mutex/Semaphore**: may sleep — calls `schedule()`. Use only in process context.

### 1.2 ARMv8 atomic primitives
- **LL/SC**: `ldxr Xt, [Xn]` (load-exclusive) + `stxr Ws, Xt, [Xn]` (store-exclusive). `Ws` = 0 on success, 1 on contention.
- **Modern LSE**: `swp`, `cas`, `ldadd` single-instruction. Available on Cortex-A72 with `-march=armv8-a+lse`.

For Day 14 (UP), a non-atomic `int locked` is technically enough, but we use `cas` to be SMP-ready.

### 1.3 Wait queue pattern
```c
wait_event(wq, condition):
    while (!condition) {
        prepare_to_wait(wq, &entry, TASK_BLOCKED);
        if (!condition) schedule();
        finish_wait(wq, &entry);
    }

wake_up(wq):
    for each entry on wq: entry->task->state = TASK_RUNNING; remove from wq; rq_add(task)
```

---

## 2. Design

### 2.1 Files
```
kernel/locking/spinlock.c
kernel/locking/mutex.c
kernel/locking/semaphore.c
kernel/sched/wait.c
include/kernel/spinlock.h
include/kernel/wait.h
include/kernel/mutex.h
```

### 2.2 Spinlock with IRQ save
```c
typedef struct { volatile u32 lock; } spinlock_t;

static inline void spin_lock(spinlock_t *s)
{
    u32 expected, one = 1;
    do {
        expected = 0;
        asm volatile(
            "1: ldxr %w0, [%2]\n"
            "   cmp  %w0, #0\n"
            "   b.ne 1b\n"
            "   stxr w1, %w3, [%2]\n"
            "   cbnz w1, 1b\n"
            : "=&r"(expected) : "Q"(*s), "r"(&s->lock), "r"(one) : "cc","w1","memory");
    } while (0);
    asm volatile("dmb ish" ::: "memory");
}

static inline void spin_unlock(spinlock_t *s)
{
    asm volatile("dmb ish" ::: "memory");
    s->lock = 0;
}

#define spin_lock_irqsave(s, f) do { \
    asm volatile("mrs %0, daif; msr daifset, #2" : "=r"(f) :: "memory"); \
    spin_lock(s); \
} while (0)

#define spin_unlock_irqrestore(s, f) do { \
    spin_unlock(s); \
    asm volatile("msr daif, %0" :: "r"(f) : "memory"); \
} while (0)
```

---

## 3. Implementation

### 3.1 `kernel/sched/wait.c`
```c
#include <kernel/wait.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/sched.h>

struct wait_entry {
    struct task *task;
    struct wait_entry *next;
};

void init_waitqueue_head(struct wait_queue_head *wq)
{
    wq->head = NULL;
    wq->lock = (spinlock_t){0};
}

void prepare_to_wait(struct wait_queue_head *wq, struct wait_entry *e, int state)
{
    unsigned long f;
    e->task = current();
    spin_lock_irqsave(&wq->lock, f);
    e->next = wq->head;
    wq->head = e;
    current()->state = state;
    spin_unlock_irqrestore(&wq->lock, f);
}

void finish_wait(struct wait_queue_head *wq, struct wait_entry *e)
{
    unsigned long f;
    spin_lock_irqsave(&wq->lock, f);
    /* unlink */
    struct wait_entry **pp = &wq->head;
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp) *pp = e->next;
    current()->state = TASK_RUNNING;
    spin_unlock_irqrestore(&wq->lock, f);
}

void wake_up(struct wait_queue_head *wq)
{
    unsigned long f;
    spin_lock_irqsave(&wq->lock, f);
    struct wait_entry *e = wq->head;
    while (e) {
        e->task->state = TASK_RUNNING;
        sched_add(e->task);
        e = e->next;
    }
    wq->head = NULL;
    spin_unlock_irqrestore(&wq->lock, f);
    need_resched = 1;
}
```

`wait_event` macro:
```c
#define wait_event(wq, cond) do { \
    struct wait_entry __e; \
    while (!(cond)) { \
        prepare_to_wait(&(wq), &__e, TASK_BLOCKED); \
        if (!(cond)) schedule(); \
        finish_wait(&(wq), &__e); \
    } \
} while (0)
```

### 3.2 Mutex (sleeping lock)
```c
struct mutex {
    spinlock_t   wait_lock;
    struct task *owner;
    struct wait_queue_head wq;
};

void mutex_init(struct mutex *m) {
    m->wait_lock = (spinlock_t){0};
    m->owner = NULL;
    init_waitqueue_head(&m->wq);
}

void mutex_lock(struct mutex *m) {
    while (1) {
        unsigned long f;
        spin_lock_irqsave(&m->wait_lock, f);
        if (!m->owner) { m->owner = current(); spin_unlock_irqrestore(&m->wait_lock, f); return; }
        spin_unlock_irqrestore(&m->wait_lock, f);
        wait_event(m->wq, m->owner == NULL);
    }
}

void mutex_unlock(struct mutex *m) {
    unsigned long f;
    spin_lock_irqsave(&m->wait_lock, f);
    m->owner = NULL;
    spin_unlock_irqrestore(&m->wait_lock, f);
    wake_up(&m->wq);
}
```

### 3.3 Semaphore (counted)
```c
struct semaphore { spinlock_t lock; int count; struct wait_queue_head wq; };

void sem_init(struct semaphore *s, int v) { s->lock=(spinlock_t){0}; s->count=v; init_waitqueue_head(&s->wq); }

void down(struct semaphore *s) {
    while (1) {
        unsigned long f; spin_lock_irqsave(&s->lock, f);
        if (s->count > 0) { s->count--; spin_unlock_irqrestore(&s->lock, f); return; }
        spin_unlock_irqrestore(&s->lock, f);
        wait_event(s->wq, s->count > 0);
    }
}

void up(struct semaphore *s) {
    unsigned long f; spin_lock_irqsave(&s->lock, f);
    s->count++;
    spin_unlock_irqrestore(&s->lock, f);
    wake_up(&s->wq);
}
```

### 3.4 Completion (one-shot)
```c
struct completion { int done; struct wait_queue_head wq; };
void complete(struct completion *c)         { c->done = 1; wake_up(&c->wq); }
void wait_for_completion(struct completion *c) { wait_event(c->wq, c->done); }
```

---

## 4. Pitfalls

1. **`dmb ish` placement**: required after acquiring and before releasing a lock to publish writes to other CPUs. Without it, SMP (Day 28) sees torn data.
2. **Lost wakeup**: `if (!cond) schedule()` MUST recheck `cond` under the wait-queue lock or you sleep forever. The `wait_event` macro does the recheck — preserve the pattern.
3. **Sleeping with spinlock held**: forbidden. Convert to mutex or release spinlock before sleeping. Add a `lockdep`-lite check Day 29.
4. **Reentrancy in `wake_up`**: if `wake_up` is called from IRQ context, it must not call `schedule` directly — just set `need_resched`. We do that here.
5. **Priority inversion**: with a single RR queue and one mutex, possible but rare. Real fix is PI mutex — out of scope.

---

## 5. Verification

```c
struct semaphore sem;
sem_init(&sem, 0);

void producer(void *_) { for (int i=0;i<5;i++){ printk("P%d\n", i); up(&sem); } }
void consumer(void *_) { for (int i=0;i<5;i++){ down(&sem); printk("C%d\n", i); } }
```
Expected: every `Pi` is followed (possibly after delay) by a `Ci`.

Stress: 10000-iter ping-pong between two threads via a mutex.

---

## 6. Stretch

- Ticket spinlock (FIFO fairness): two 16-bit counters, `cur` and `next`.
- Read-write semaphore.
- `wait_event_timeout` using a future `add_timer()`.

---

## 7. References

- ARM ARM §B2.9 (synchronization), §C6.2.108 (`STXR`).
- Linux `kernel/locking/qspinlock.c` (advanced — read for ideas).
- *Is Parallel Programming Hard* (Paul McKenney) ch. 7.
