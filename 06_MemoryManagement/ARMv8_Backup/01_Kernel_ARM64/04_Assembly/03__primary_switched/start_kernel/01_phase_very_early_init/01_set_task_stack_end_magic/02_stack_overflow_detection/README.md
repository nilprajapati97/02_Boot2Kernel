# Stack Overflow Detection in the Linux Kernel

## Overview

The Linux kernel uses a **multi-layered** approach to detect and prevent kernel stack overflows.

---

## Layer 1: Stack End Magic (Always Enabled)

As described in the parent document, `STACK_END_MAGIC = 0x57AC6E9D` is placed at `end_of_stack(task)`. Checked via:
```c
static inline int task_stack_end_corrupted(struct task_struct *p)
{
    return unlikely(*end_of_stack(p) != STACK_END_MAGIC);
}
```

**Checked at:**
- Syscall return path (`exit_to_user_mode_prepare`)
- `/proc/<pid>/stat` reads
- Stack unwinding

**Limitation:** Only detects corruption **after** the fact, does not prevent it.

---

## Layer 2: Virtual Guard Pages (`CONFIG_VMAP_STACK`)

Since Linux 4.9, kernel stacks can be **virtually mapped** (`vmalloc`-backed). This means a **virtual guard page** (unmapped) sits below the stack. Any access past the stack bottom causes an immediate **page fault** (or NMI on x86), not a silent corruption.

```
High addr
┌──────────────────┐ ← RSP starts here
│  kernel stack    │
│  (grows down)    │
│                  │
│  ↓  ↓  ↓        │
├──────────────────┤ ← end_of_stack() — MAGIC here
│  GUARD PAGE      │ ← NOT mapped (access = page fault)
└──────────────────┘
Low addr
```

**On guard page hit:**
- If stack overflow is caught during normal execution → `do_page_fault` → `handle_stack_overflow()` → kernel panic
- If stack overflow happens during NMI/MCE → handled on separate IST stack → can print backtrace before panic

---

## Layer 3: KASAN Stack (CONFIG_KASAN)

Kernel Address Sanitizer tracks **every stack access** via shadow memory:
- Detects out-of-bounds within a single stack frame
- Detects use-after-scope (using a local variable after its scope ends)
- Detects stack buffer overflows (e.g., `char buf[8]; buf[8] = 0`)

KASAN doubles effective stack size → `THREAD_SIZE` must be doubled when KASAN is enabled.

---

## Layer 4: Per-CPU Interrupt Stacks (x86)

x86-64 has **7 IST (Interrupt Stack Table)** entries in the TSS, each with its own independent stack:
- `DOUBLEFAULT_STACK` — for double faults (including stack overflow handler!)
- `NMI_STACK` — Non-Maskable Interrupt
- `DEBUG_STACK` — Debug exceptions (INT1)
- `MCE_STACK` — Machine Check Exception

These separate stacks mean that even if the task kernel stack overflows, the hardware automatically switches to a safe IST stack for critical exceptions.

---

## Runtime Stack Usage Monitoring

```c
// include/linux/sched/task_stack.h
static inline unsigned long stack_not_used(struct task_struct *p)
{
    unsigned long *n = end_of_stack(p);
    do {    /* Skip over canary */
        n++;
    } while (!*n);
    return (unsigned long)n - (unsigned long)end_of_stack(p);
}
```

`/proc/<pid>/status` shows `VmStk` — the kernel stack size in use.

---

## Interview Q&A

### Q1: What is the difference between `VMAP_STACK` and the stack end magic?
**A:** Stack end magic is a **detection** mechanism — it tells you a stack overflow already happened. `VMAP_STACK` is a **prevention + detection** mechanism — the virtual guard page causes a fault the moment the stack would overflow, before any corruption occurs. `VMAP_STACK` is strictly superior but costs virtual address space (one guard page = 4KB per thread). The magic is kept as a low-cost backup for configurations without `VMAP_STACK`.

### Q2: How does the kernel print a backtrace when a stack overflow occurs if the stack is broken?
**A:** This is why IST (Interrupt Stack Table) stacks exist on x86. The `#DF` (double fault) handler uses `DOUBLEFAULT_STACK` — a completely separate stack. When a stack overflow causes a page fault, and that page fault handler itself overflows (double fault), the CPU automatically switches to `DOUBLEFAULT_STACK` using the TSS. From this safe stack, the kernel can call `dump_stack()` to walk and print the overflowed task's stack (backtrace from a safe vantage point).

### Q3: How does stack overflow affect SMP systems differently than UP?
**A:** On SMP, each CPU has its own set of stacks: task kernel stack, interrupt stack, softirq stack, IST stacks. A stack overflow on one CPU only affects that CPU's current task — other CPUs continue running. However, if the overflow corrupts `thread_info.flags` and the overflowing task later returns to userspace on a different CPU (after migration), the corrupted flags could affect a different CPU. `VMAP_STACK` mitigates this by causing an immediate local fault before corruption propagates.
