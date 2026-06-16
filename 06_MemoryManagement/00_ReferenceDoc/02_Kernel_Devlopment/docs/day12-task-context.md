# Day 12 — `task_struct` & Context Switch

> **Goal**: Define `struct task` (Linux's `task_struct` analog), allocate kernel stacks, and implement `cpu_switch_to(prev, next)` in assembly that saves/restores callee-saved registers + SP + FP.
>
> **Why today**: Without a saved-context structure and a switcher routine, you cannot have more than one thread. Day 13 builds the scheduler that calls this primitive.

---

## 1. Background

### 1.1 AAPCS64 callee-saved set
A function may freely clobber `x0..x18` (caller-saved). It must preserve:
- `x19..x28` (10 general-purpose)
- `x29` (FP), `x30` (LR)
- `SP`
- FP/SIMD registers `d8..d15` (low 64 bits) — we don't touch FP in kernel paths, so skip.

Thus a context switch needs to save exactly these on `prev`'s stack and restore from `next`'s.

### 1.2 Kernel stack layout
- One 16 KiB stack per task, allocated from buddy (`alloc_pages(2)`).
- Top of stack stores the task's `cpu_context` struct (saved x19..x30, sp).
- Bottom guards against overflow with a magic value (`STACK_END_MAGIC`).

### 1.3 The `current` macro
In Linux, `current` is derived from SP: mask off the stack-size to get the bottom, which begins with `struct thread_info` (or `task_struct`). We use a simpler approach: dedicate `sp_el0` to hold the current `task *` pointer in kernel context — it's free in EL1 because we never run with SP_EL0 there. (Linux also uses this trick on arm64.)

---

## 2. Design

### 2.1 Files
```
include/kernel/sched.h
include/kernel/task.h
kernel/sched/core.c           (alloc/init/destroy task)
arch/arm64/kernel/switch.S    (cpu_switch_to)
```

### 2.2 Structs
```c
struct cpu_context {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    u64 fp;          /* x29 */
    u64 sp;
    u64 pc;          /* x30 at the time of save */
};

struct task {
    struct cpu_context ctx;          /* MUST be offset 0 for asm */
    int    pid;
    int    state;                    /* TASK_RUNNING, _BLOCKED, _ZOMBIE */
    int    prio;
    u64    counter;                  /* RR remaining ticks */
    void  *kstack;                   /* base of 16 KiB */
    struct mm_struct *mm;            /* NULL for kthreads */
    struct task *next;               /* runqueue link */
    char   comm[16];
};

#define TASK_RUNNING  0
#define TASK_BLOCKED  1
#define TASK_ZOMBIE   2
```

---

## 3. Implementation

### 3.1 `arch/arm64/kernel/switch.S`
```asm
/* x0 = prev *task, x1 = next *task; cpu_context is at offset 0 */
    .global cpu_switch_to
cpu_switch_to:
    mov     x10, sp
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x10, [x0, #80]      // fp, sp
    str     x30,      [x0, #96]      // pc = LR

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x10, [x1, #80]
    ldr     x30,      [x1, #96]
    mov     sp, x10
    /* update sp_el0 (current task pointer) */
    msr     sp_el0, x1
    ret                              // returns to LR = next->ctx.pc
```

### 3.2 `current` helper
```c
static inline struct task *current(void)
{
    struct task *t;
    asm volatile("mrs %0, sp_el0" : "=r"(t));
    return t;
}
```

### 3.3 `kernel/sched/core.c` (today: task creation)
```c
#include <kernel/task.h>
#include <kernel/slab.h>
#include <kernel/page.h>
#include <kernel/printk.h>

#define KSTACK_ORDER 2                  /* 16 KiB */
#define KSTACK_SIZE  (PAGE_SIZE << KSTACK_ORDER)

static int next_pid = 1;

extern void cpu_switch_to(struct task *prev, struct task *next);

/* The trampoline runs with x0 = arg, lr = task entry */
extern void ret_from_kthread(void);

struct task *task_create(void (*fn)(void *), void *arg, const char *name)
{
    struct task *t = kmalloc(sizeof *t, 0);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);

    phys_addr_t pa = alloc_pages(KSTACK_ORDER);
    if (!pa) { kfree(t); return NULL; }
    void *stack = (void *)(pa + 0xffff000000000000UL);
    t->kstack = stack;

    /* Set up initial context so first switch lands inside ret_from_kthread,
       which calls fn(arg) and then exits.                                  */
    u64 *sp = (u64 *)((u8 *)stack + KSTACK_SIZE);
    t->ctx.sp = (u64)sp;
    t->ctx.pc = (u64)ret_from_kthread;
    t->ctx.x19 = (u64)fn;
    t->ctx.x20 = (u64)arg;
    t->pid = next_pid++;
    t->state = TASK_RUNNING;
    t->counter = 4;             /* RR quantum */
    strncpy(t->comm, name, sizeof t->comm - 1);
    return t;
}
```

### 3.4 `ret_from_kthread` trampoline
```asm
    .global ret_from_kthread
ret_from_kthread:
    mov     x0, x20             // arg
    blr     x19                 // call fn(arg)
    bl      do_exit             // never returns
1:  b       1b
```

### 3.5 Minimal manual test (no scheduler yet)
```c
static struct task *idle, *a;

static void hello_thread(void *arg)
{
    for (int i = 0; i < 5; i++) printk("[%s] iter %d\n", current()->comm, i);
    /* yield manually for test */
    cpu_switch_to(current(), idle);
}

void test_switch(void)
{
    idle = kmalloc(sizeof *idle, 0); memset(idle, 0, sizeof *idle);
    strncpy(idle->comm, "idle", 5);
    asm volatile("msr sp_el0, %0" :: "r"(idle));   /* current = idle */

    a = task_create(hello_thread, NULL, "A");
    cpu_switch_to(idle, a);    /* one-way for test; control returns when 'a' yields */
    printk("back in idle\n");
}
```

---

## 4. ARM64 Cheat-Sheet (Day 12)

```
sp_el0        unused at EL1 → repurposed as 'current' pointer
stp/ldp       pair store/load with #-prefixed signed offset
ret           branch to LR (x30)
blr Xn        branch with link to register
```

---

## 5. Pitfalls

1. **`cpu_context` offset != 0**: the asm uses raw offsets. Generate `asm-offsets.h` (preferably) or `static_assert(offsetof(struct task, ctx) == 0)` in C.
2. **Stack alignment**: top of stack must be 16-byte aligned. `KSTACK_SIZE` is a multiple of 16, base is page-aligned → top is aligned. Good.
3. **First-switch trap**: on the very first switch from "no context" you can't save into a real task. Solution: have a bootstrap `init_task` statically allocated; first switch always uses it as `prev`.
4. **Caller-saved regs across switch**: `cpu_switch_to` is called as a normal C function — caller already saved x0..x18 it cared about. We only must preserve callee-saved.
5. **TLB & MMU on switch**: when switching to a user task with its own page tables (Day 15), also load `TTBR0_EL1` and issue `tlbi aside1is, ASID`. Kernel threads share `TTBR1` — no TLBI needed.

---

## 6. Verification

```
[idle] -> switch to A
[A] iter 0
[A] iter 1
…
back in idle
```

GDB: break in `cpu_switch_to`, inspect saved/restored regs; check `sp_el0` matches new task ptr.

---

## 7. Stretch

- Save FP/SIMD lazily — set `CPACR_EL1.FPEN=0`, on first FP-trap save into `task->fpsimd` and re-enable for that task.
- Add `task->thread_info.preempt_count` for nesting later.
- Mark stack with `STACK_END_MAGIC` and check on context switch.

---

## 8. References

- ARM AAPCS64 (IHI 0055).
- Linux `arch/arm64/kernel/entry.S` `cpu_switch_to`.
- Linux `arch/arm64/include/asm/current.h`.
