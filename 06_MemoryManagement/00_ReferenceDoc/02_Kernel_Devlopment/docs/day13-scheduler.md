# Day 13 — Round-Robin Scheduler & Idle

> **Goal**: Wire a global Round-Robin scheduler driven by the timer tick. Implement `schedule()`, `need_resched`, idle task using `wfi`, and spawn two kernel threads printing "A"/"B".
>
> **Why today**: This is the moment "kernel" becomes "multitasking kernel". Everything after (process creation Day 15, EL0 drop Day 16) relies on `schedule()`.

---

## 1. Background

### 1.1 Three places `schedule()` runs
1. **Voluntary**: a kernel thread calls `schedule()` (e.g., after blocking on a wait queue).
2. **On return to user (Day 16)**: if `need_resched` is set, call before `eret`.
3. **Preemption from IRQ**: timer tick decrements `current->counter`; when it hits zero, set `need_resched`; on IRQ exit, if returning to a preemptible context, call `schedule()`.

### 1.2 Why preemption only on IRQ-exit
Calling `schedule()` from inside the timer handler directly would context-switch while holding the IRQ-stack — gnarly with reentrancy. Standard pattern: handler sets a flag; after `kernel_exit` macro restores state, we check the flag and `bl schedule`.

### 1.3 The idle task
- PID 0.
- Loop: `for(;;) { enable_irq(); wfi; }`.
- Never blocks; always runnable so the runqueue is never empty.

---

## 2. Design

### 2.1 Files
```
kernel/sched/core.c        (schedule, runqueue)
kernel/sched/rr.c          (pick_next_task)
include/kernel/sched.h
```

### 2.2 Runqueue
Single doubly-linked list (circular) for simplicity. Per-CPU version on Day 28.

```c
static struct task *rq_head;        /* first runnable */
static volatile int need_resched;
```

---

## 3. Implementation

### 3.1 `kernel/sched/core.c`
```c
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/printk.h>

extern void cpu_switch_to(struct task *prev, struct task *next);

static struct task *rq_head;
static struct task *idle_task;
volatile int need_resched;

static void rq_add(struct task *t)
{
    if (!rq_head) { rq_head = t; t->next = t; return; }
    t->next = rq_head->next;
    rq_head->next = t;
}

static void rq_remove(struct task *t)
{
    if (!rq_head) return;
    struct task *p = rq_head;
    while (p->next != t && p->next != rq_head) p = p->next;
    if (p->next == t) {
        p->next = t->next;
        if (rq_head == t) rq_head = (t->next == t) ? NULL : t->next;
    }
}

/* RR: pick next runnable after current */
static struct task *pick_next(struct task *cur)
{
    if (!rq_head) return idle_task;
    struct task *p = cur->next ? cur->next : rq_head;
    struct task *start = p;
    do {
        if (p->state == TASK_RUNNING) return p;
        p = p->next ? p->next : rq_head;
    } while (p != start);
    return idle_task;
}

void schedule(void)
{
    struct task *prev = current();
    struct task *next = pick_next(prev);
    need_resched = 0;
    if (next == prev) return;
    cpu_switch_to(prev, next);
}

/* called from timer tick */
void scheduler_tick(void)
{
    struct task *t = current();
    if (t == idle_task) { need_resched = 1; return; }
    if (--t->counter <= 0) {
        t->counter = 4;
        need_resched = 1;
    }
}

void sched_init(void)
{
    extern struct task *task_create(void (*)(void *), void *, const char *);
    idle_task = kmalloc(sizeof *idle_task, 0);
    memset(idle_task, 0, sizeof *idle_task);
    strncpy(idle_task->comm, "swapper", 7);
    idle_task->state = TASK_RUNNING;
    asm volatile("msr sp_el0, %0" :: "r"(idle_task));
}

void sched_add(struct task *t) { rq_add(t); }
```

### 3.2 Updated `do_irq` (exception exit checks `need_resched`)
In `entry.S` kernel_exit macro, before `eret`, we check the flag if we're returning to EL1 (kernel-mode preemption) **and** preempt is enabled:

```asm
    .macro kernel_exit, el
    /* … restore registers as before, just before eret … */
    .if \el == 1
        bl      preempt_if_needed
    .endif
    /* … standard restore … */
    eret
    .endm
```

`preempt_if_needed`:
```c
void preempt_if_needed(void)
{
    if (need_resched) schedule();
}
```

> For Day 13 we don't enable kernel preemption deeply — simpler: only check `need_resched` when returning from IRQ to a kthread main loop. A safe `cond_resched()` helper:
> ```c
> void cond_resched(void) { if (need_resched) schedule(); }
> ```
> kthreads can call `cond_resched()` in their loop.

### 3.3 Hook into timer
```c
static void timer_tick(unsigned irq)
{
    write_tval(timer_freq / HZ);
    jiffies++;
    scheduler_tick();
}
```

### 3.4 The two test threads
```c
static void thr_a(void *_) {
    while (1) { printk("A "); for (volatile int i=0;i<1000000;i++); cond_resched(); }
}
static void thr_b(void *_) {
    while (1) { printk("B "); for (volatile int i=0;i<1000000;i++); cond_resched(); }
}

void kmain_post_mmu(void) {
    /* ... earlier init ... */
    sched_init();
    sched_add(task_create(thr_a, NULL, "A"));
    sched_add(task_create(thr_b, NULL, "B"));
    gic_init(); timer_init();
    asm volatile("msr daifclr, #2");
    /* become idle */
    for (;;) { asm volatile("wfi"); cond_resched(); }
}
```

---

## 4. Pitfalls

1. **Scheduling with IRQs unmasked at the wrong moment** → re-entrant `schedule`. Disable IRQ at the very start of `schedule()`, restore on exit: `local_irq_save(flags)` / `local_irq_restore(flags)`.
2. **Switching from inside the IRQ handler**: don't. Set the flag; check on IRQ-exit path.
3. **Idle task scheduled too eagerly**: it has to be lowest priority. With a single RR queue, just ensure idle is *not* on the runqueue — fallback only when empty.
4. **`current` stale after switch**: since we use `sp_el0`, `cpu_switch_to` updates it in asm — verify no C code caches `current()` across a switch.
5. **`wfi` with IRQs masked** = deadlock. `wfi` does **not** wake with PSTATE.I set unless the IRQ is at higher priority than the boundary. Always unmask before `wfi`.

---

## 5. Verification (Phase 3 partial)

```
A B A B A B A B …   (roughly alternating, governed by 4-tick quantum)
```

Add a counter: after 1000 alternations, print `sched ok`. Wire into CI smoke.

GDB: at any point, `bt` should show either thread A's or thread B's call chain; `sp_el0` matches the task pointer.

---

## 6. Stretch

- Add `MLFQ` (multi-level feedback): four priority queues, decay on quantum exhaustion.
- Implement `cond_resched_lock(lock)` that drops the lock around the reschedule.
- Per-CPU runqueue stub (single CPU for now) — easy migration to Day 28.

---

## 7. References

- Linux `kernel/sched/core.c`, `kernel/sched/fair.c` (read CFS for inspiration only).
- xv6 `proc.c` `scheduler()`.
- Tanenbaum *Modern Operating Systems* §2.4.
