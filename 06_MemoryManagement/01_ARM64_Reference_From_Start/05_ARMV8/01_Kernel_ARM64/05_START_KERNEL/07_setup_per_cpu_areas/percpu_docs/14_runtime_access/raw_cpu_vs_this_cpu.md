# `raw_cpu_*` vs `this_cpu_*` vs `__this_cpu_*`: Preemption Safety Guide

## Source Reference
- `include/linux/percpu-defs.h` — all per-CPU access macros
- `include/linux/percpu.h` — includes percpu-defs.h
- `kernel/sched/core.c` — preempt_disable/enable implementation

---

## Overview: Three Levels of Per-CPU Access

The Linux kernel provides three families of per-CPU access macros, differing
only in their preemption-safety guarantees:

```
Level 1: this_cpu_*      → No automatic preemption disable; caller must ensure safety
Level 2: __this_cpu_*    → For use in interrupt/NMI context (preemption already off)
Level 3: raw_cpu_*       → No preemption checks; even in DEBUG_PREEMPT mode
         get_cpu_var()   → Automatically calls preempt_disable() before access
```

---

## What "Preemption Safety" Means for Per-CPU

```
Timeline without preemption control:

CPU 0:                              CPU 1:
  ptr = this_cpu_ptr(&my_var)       [task was migrated here by scheduler]
  ; ptr = &my_var + offset[CPU0]   
                              ← preemption here (task moves to CPU1)
  *ptr += 1;                        ; modifying CPU0's copy from CPU1!
                                    ; CPU0 may be using same memory concurrently
```

**Rule:** You must prevent task migration between obtaining `this_cpu_ptr()` and
using the pointer. Any of these mechanisms suffice:
- `preempt_disable()` / `preempt_enable()`
- `local_irq_save()` / `local_irq_restore()`
- Holding a spinlock
- Being in interrupt context (IRQ/NMI always runs on a fixed CPU)

---

## `this_cpu_*` — Standard API

```c
/* No automatic preemption control; caller is responsible. */
/* In DEBUG_PREEMPT mode, checks that preemption IS disabled. */

this_cpu_ptr(&var)          /* pointer to current CPU's var */
this_cpu_read(var)          /* read current CPU's var */
this_cpu_write(var, val)    /* write current CPU's var */
this_cpu_add(var, val)      /* add val to current CPU's var */
this_cpu_sub(var, val)      /* subtract */
this_cpu_inc(var)           /* increment */
this_cpu_dec(var)           /* decrement */
this_cpu_or(var, val)       /* bitwise OR */
this_cpu_and(var, val)      /* bitwise AND */
this_cpu_xchg(var, val)     /* exchange */
this_cpu_cmpxchg(var, o, n) /* compare and exchange */
```

### When to Use

```c
/* Example: hot path counter in softirq (preemption disabled) */
void net_rx_softirq(void)
{
    /* softirq = preemption disabled */
    this_cpu_inc(net_stats.packets);   /* safe: softirq can't migrate */
}

/* Example: hot path with explicit disable */
void update_per_cpu_stat(int val)
{
    preempt_disable();
    this_cpu_add(my_stat, val);        /* safe */
    preempt_enable();
}
```

---

## `__this_cpu_*` — IRQ/NMI Context API

```c
/* For use in interrupt handlers and NMI context.
 * Preemption is ALWAYS disabled in interrupt context (hardware guarantees it).
 * __this_cpu_* skips the preemption check assertion.
 * Even in DEBUG_PREEMPT builds, no check is performed.
 */

__this_cpu_ptr(&var)
__this_cpu_read(var)
__this_cpu_write(var, val)
__this_cpu_add(var, val)
__this_cpu_inc(var)
/* ... same set as this_cpu_* but without DEBUG_PREEMPT assertions */
```

### When to Use

```c
/* IRQ handler: guaranteed to run on fixed CPU */
static irqreturn_t my_irq_handler(int irq, void *dev)
{
    __this_cpu_inc(irq_count);    /* no preempt check needed */
    return IRQ_HANDLED;
}

/* NMI handler: hardest non-maskable interrupt */
void nmi_handler(void)
{
    __this_cpu_add(nmi_stats.count, 1);
}
```

**Important:** `__this_cpu_*` in task context (where preemption IS enabled) is
a bug. The `__` prefix means "I know preemption is off; skip the debug check."
Using it in preemptible context is an un-caught race condition.

---

## `raw_cpu_*` — Absolutely No Checks

```c
/* No preemption disable.
 * No DEBUG_PREEMPT assertions.
 * Use only in very specific low-level code where you can guarantee safety
 * through other means.
 */

raw_cpu_ptr(&var)
raw_cpu_read(var)
raw_cpu_write(var, val)
raw_cpu_add(var, val)
raw_cpu_inc(var)
```

### When to Use

```c
/* During early boot before preemption infrastructure is set up */
void __init early_setup(void)
{
    raw_cpu_write(cpu_number, boot_cpu_id);  /* preemption not yet possible */
}

/* In assembly-generated stubs or very performance-critical paths */
/* where all safety is guaranteed by surrounding context */
```

---

## `get_cpu_var()` / `put_cpu_var()` — Automatic Safe Access

```c
#define get_cpu_var(var)    \
({                          \
    preempt_disable();      \  /* ← automatic preempt disable */
    this_cpu_var(var);      \
})

#define put_cpu_var(var)    \
({                          \
    (void)&(var);           \
    preempt_enable();       \  /* ← automatic preempt enable */
})
```

### Usage Pattern

```c
/* For struct/complex type modifications: */
struct my_data {
    int count;
    unsigned long timestamp;
};
DEFINE_PER_CPU(struct my_data, my_percpu_data);

/* Correct usage: */
struct my_data *data = &get_cpu_var(my_percpu_data);
data->count++;
data->timestamp = jiffies;
put_cpu_var(my_percpu_data);

/* WARNING: Never return from between get/put! */
/* WRONG: */
struct my_data *data = &get_cpu_var(my_percpu_data);
if (data->count > 100)
    return -EBUSY;   /* BUG: preempt_enable() never called! */
put_cpu_var(my_percpu_data);
```

---

## `get_cpu_ptr()` / `put_cpu_ptr()` — Pointer-Based Safe Access

```c
/* Like get_cpu_var but returns a pointer */
#define get_cpu_ptr(var)                    \
({                                          \
    preempt_disable();                      \
    this_cpu_ptr(var);                      \
})

#define put_cpu_ptr(var)    (preempt_enable())

/* Usage: */
struct my_struct *p = get_cpu_ptr(&my_percpu_struct);
p->field = value;
put_cpu_ptr(&my_percpu_struct);
```

---

## Comparison Table

| Macro Family | Preempt Disable? | Debug Check? | Use Case |
|---|---|---|---|
| `this_cpu_*` | No (caller) | Yes (in DEBUG_PREEMPT) | Normal task code with explicit preempt management |
| `__this_cpu_*` | No (caller) | No | IRQ/NMI handlers, guaranteed-atomic contexts |
| `raw_cpu_*` | No (caller) | No | Early boot, very low-level, perf-critical |
| `get_cpu_var()` | Yes (builtin) | N/A | Convenient task code with automatic safety |
| `per_cpu_ptr(ptr, cpu)` | No | N/A | Accessing another CPU's data |

---

## Interrupt Context Safety Matrix

```
Context              | this_cpu_* | __this_cpu_* | raw_cpu_* | get_cpu_var()
---------------------|------------|--------------|-----------|---------------
Task (preemptible)   |  UNSAFE*   |   UNSAFE*    |  UNSAFE*  |    SAFE
Task (preempt_dis.)  |   SAFE     |    SAFE      |   SAFE    |    SAFE
Softirq              |   SAFE     |    SAFE      |   SAFE    |    SAFE†
Hardirq (IRQ)        |   SAFE     |    SAFE      |   SAFE    |    SAFE†
NMI                  |   SAFE     |    SAFE      |   SAFE    |  UNSAFE‡

* = architecturally unsafe (race window), debug build will catch with this_cpu_*
† = get_cpu_var in IRQ context is safe but wastes a preempt_disable() call
‡ = NMI can interrupt get_cpu_var between preempt_disable and the access
```

---

## Key Design Principle: Why Per-CPU Needs No Locks

```c
/* Traditional shared data (needs lock): */
atomic_t global_counter;
atomic_inc(&global_counter);   /* atomic operation needed */

/* Per-CPU data (no lock needed): */
DEFINE_PER_CPU(int, local_counter);
this_cpu_inc(local_counter);   /* no lock: only this CPU writes */
```

**The guarantee:** CPU N can ONLY modify `__per_cpu_offset[N]`'s data via
`this_cpu_*()`. No other CPU will ever write to that same memory location.
The only time another CPU reads CPU N's per-CPU data is via `per_cpu_ptr(ptr, N)` —
a cross-CPU read, which must use appropriate memory ordering if synchronization
with CPU N's writes is needed.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Why no lock for per-CPU writes? | Only one CPU writes its own per-CPU copy |
| When is `__this_cpu_*` appropriate? | IRQ/NMI context where preemption is hardware-guaranteed off |
| What does `get_cpu_var()` do automatically? | Calls `preempt_disable()` before and `preempt_enable()` after |
| Can `this_cpu_*` be used in NMI? | Unsafe — NMI can interrupt a get_cpu_var() preempt_disable() |
| What is a "preemption window"? | The gap between reading the per-CPU pointer and using it |
| When is per-CPU counter access lock-free? | Always — exclusive ownership eliminates need for synchronization |
| What is `raw_cpu_*` for? | Bypasses all assertions; for early boot or guaranteed-safe hot paths |
