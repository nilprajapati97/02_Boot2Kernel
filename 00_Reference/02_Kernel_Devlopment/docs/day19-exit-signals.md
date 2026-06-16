# Day 19 — Process Lifecycle & Signal Stub

> **Goal**: Implement `do_exit`, zombie state, `wait4`, parent/child links + reparenting to init; minimal kill-class signals (SIGSEGV/SIGKILL → terminate; SIGCHLD → notify parent). No user signal handlers yet.
>
> **Why today**: Shell (Day 26) forks children and `wait4`s them. ELF loader (Day 18) tasks return via `exit`. Page fault path (Day 11) needs to kill misbehaving processes.

---

## 1. Background

### 1.1 Lifecycle states
```
TASK_RUNNING ──schedule()──> ... ──schedule()──> TASK_RUNNING
TASK_RUNNING ──do_exit──> TASK_ZOMBIE ──wait4──> freed
TASK_RUNNING ──prepare_to_wait──> TASK_BLOCKED ──wake_up──> TASK_RUNNING
```

A **zombie** keeps its `task` allocated (so parent can read exit code) but releases mm, kstack, files.

### 1.2 Parent/child structure
```c
struct task {
    /* ... Day 12 fields ... */
    struct task *parent;
    struct task *first_child;
    struct task *sibling;          /* among same parent */
    int exit_code;
    struct wait_queue_head waiters; /* parent waits here */
    u32 pending_signals;            /* bitmap by signal nr */
};
```

### 1.3 Reparenting
When a parent dies before its children, children's `parent` pointer is moved to `init` (pid 1). Init is responsible for reaping orphans.

### 1.4 Signal model (simplified)
- 64-bit `pending_signals` bitmap.
- On context switch *return-to-user*, check for pending fatal signals. If `SIGKILL`/`SIGSEGV`/`SIGTERM` (no handler) → `do_exit(signo)`.
- `kill(pid, sig)` just sets the bit.
- Day 19 implements no user-side handlers; full delivery is a stretch goal beyond 30 days.

---

## 2. Design

### 2.1 Files
```
kernel/exit.c             (do_exit, wait4)
kernel/signal.c           (kill, signal_pending, do_signal)
include/kernel/signal.h
```

### 2.2 New syscalls
| # | Name | Day |
|---|---|---|
| 129 | `kill` | 19 |
| 260 | `wait4` | 19 |
| 220 | `clone` | 19 (fork via flags=0) |

---

## 3. Implementation

### 3.1 Process tree helpers
```c
static void link_child(struct task *p, struct task *c)
{
    c->parent = p;
    c->sibling = p->first_child;
    p->first_child = c;
}

static void unlink_child(struct task *p, struct task *c)
{
    struct task **pp = &p->first_child;
    while (*pp && *pp != c) pp = &(*pp)->sibling;
    if (*pp) *pp = c->sibling;
}
```
Set in `do_fork`:
```c
link_child(parent, child);
init_waitqueue_head(&child->waiters);
```

### 3.2 `kernel/exit.c`
```c
extern struct task *init_task;          /* PID 1 */

__attribute__((noreturn))
void do_exit(int code)
{
    struct task *t = current();
    t->exit_code = code;

    /* Reparent children to init */
    while (t->first_child) {
        struct task *c = t->first_child;
        unlink_child(t, c);
        link_child(init_task, c);
    }

    /* Release mm + files (we don't have files yet) */
    if (t->mm) { mm_free(t->mm); t->mm = NULL; }

    /* Become zombie */
    t->state = TASK_ZOMBIE;

    /* Notify parent via SIGCHLD + wake their waitqueue */
    if (t->parent) {
        t->parent->pending_signals |= (1ULL << 17 /* SIGCHLD */);
        wake_up(&t->parent->waiters);
    }

    /* Remove from runqueue; schedule another task forever */
    rq_remove(t);
    for (;;) schedule();
}

/* sys_wait4(pid, status*, options, rusage*) */
long sys_wait4(struct pt_regs *r)
{
    int wpid = (int)r->regs[0];
    int __user *status = (void *)r->regs[1];

    struct task *me = current();
    while (1) {
        struct task *z = NULL;
        for (struct task *c = me->first_child; c; c = c->sibling) {
            if (wpid > 0 && c->pid != wpid) continue;
            if (c->state == TASK_ZOMBIE) { z = c; break; }
        }
        if (z) {
            int pid = z->pid;
            int st  = (z->exit_code & 0xff) << 8;
            unlink_child(me, z);
            /* free kstack + task */
            free_pages((u64)z->kstack - 0xffff000000000000UL, KSTACK_ORDER);
            kfree(z);
            if (status) copy_to_user(status, &st, sizeof(int));
            return pid;
        }
        if (!me->first_child) return -10;     /* -ECHILD */
        /* sleep */
        wait_event(me->waiters, /* recheck */ ({
            int has_z = 0;
            for (struct task *c = me->first_child; c; c = c->sibling)
                if (c->state == TASK_ZOMBIE) { has_z = 1; break; }
            has_z;
        }));
    }
}
```

### 3.3 `kernel/signal.c`
```c
#include <kernel/signal.h>

long sys_kill(struct pt_regs *r)
{
    int pid = (int)r->regs[0], sig = (int)r->regs[1];
    if (sig < 1 || sig > 31) return -22;
    /* find task by pid (linear scan; replace with hash later) */
    extern struct task *find_task_by_pid(int);
    struct task *t = find_task_by_pid(pid);
    if (!t) return -3;
    t->pending_signals |= (1ULL << sig);
    if (t->state == TASK_BLOCKED) { t->state = TASK_RUNNING; sched_add(t); }
    return 0;
}

static int fatal_sig(int sig)
{
    return sig == 9 /*SIGKILL*/ || sig == 11 /*SIGSEGV*/ ||
           sig == 6 /*SIGABRT*/ || sig == 15 /*SIGTERM*/;
}

/* Called on every return-to-user from kernel (in entry.S kernel_exit 0 prologue) */
void do_signal(struct pt_regs *r)
{
    struct task *t = current();
    u64 pend = t->pending_signals;
    if (!pend) return;
    for (int s = 1; s < 32; s++) {
        if (!(pend & (1ULL << s))) continue;
        t->pending_signals &= ~(1ULL << s);
        if (fatal_sig(s)) {
            printk("pid %d killed by signal %d\n", t->pid, s);
            do_exit(128 + s);
        }
        /* SIGCHLD/etc just clear */
    }
}
```

### 3.4 Hook into entry.S
Before `kernel_exit 0`, add:
```asm
    .if \el == 0
        mov x0, sp
        bl  do_signal
    .endif
```

### 3.5 SIGSEGV delivery from page-fault path
In `do_page_fault` (Day 11), replace `do_exit(11)` with:
```c
current()->pending_signals |= (1ULL << 11);
```
`do_signal` on return-to-user delivers it.

---

## 4. Pitfalls

1. **Freeing the current task's kstack inside `do_exit`**: you'd pull the rug from under your own SP. Defer to parent's `wait4`. Today we already do this — only the parent frees the zombie.
2. **Signal masking races**: setting bit is atomic? On AArch64, single-word `ldr/orr/str` is *not* atomic against another CPU. Use `ldxr/stxr` or `cas`. Single-CPU for now → fine.
3. **Init exits**: kernel panic. Add `BUG_ON(t == init_task)` in `do_exit`.
4. **`wait4` deadlock**: if no children at all, return `-ECHILD` immediately. If children exist but none zombie, sleep on `me->waiters`.
5. **Zombie scan O(N)**: fine for our small fleet.

---

## 5. Verification

```c
int pid = do_fork(0, 0, child_fn, NULL);
if (pid) { int st; sys_wait4_kernel(pid, &st); printk("child %d exited %d\n", pid, st); }
```

End-to-end: hello.elf loaded by `execve`, hits `exit(0)`, parent (`init`) `wait4`s → prints `reaped pid=N`.

Trigger SIGSEGV: a user blob that does `str x0, [xzr]` → kernel logs `page fault`, `pid X killed by signal 11`, parent reaps.

---

## 6. Stretch

- Implement `sigaction` with `SA_RESTART`, save/restore mask on entry.
- Set up user-side `sa_handler` invocation: copy a signal frame onto the user stack, redirect `ELR` to handler, restore via `sigreturn` syscall.
- `setpgid`/`getpgid` + process groups for job control (`ksh` Day 26 stretch).

---

## 7. References

- POSIX.1-2017 §2.4 (Signal Concepts).
- Linux `kernel/exit.c`, `kernel/signal.c`.
- xv6 `proc.c` `exit`, `wait`.
