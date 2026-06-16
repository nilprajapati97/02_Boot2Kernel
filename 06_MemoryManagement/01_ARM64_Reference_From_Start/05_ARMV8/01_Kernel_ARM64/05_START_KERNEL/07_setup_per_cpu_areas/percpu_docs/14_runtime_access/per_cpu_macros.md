# Per-CPU Macros: `DEFINE_PER_CPU`, `per_cpu()`, `SHIFT_PERCPU_PTR`

## Source Reference
- `include/linux/percpu-defs.h:49` — `__PCPU_ATTRS()`
- `include/linux/percpu-defs.h:90` — `DEFINE_PER_CPU_SECTION()`
- `include/linux/percpu-defs.h:114` — `DEFINE_PER_CPU()`
- `include/linux/percpu-defs.h:230` — `SHIFT_PERCPU_PTR()`
- `include/linux/percpu-defs.h:233` — `per_cpu_ptr()`
- `include/linux/percpu-defs.h:250` — `this_cpu_ptr()`
- `include/linux/percpu-defs.h:269` — `per_cpu()`
- `include/asm-generic/percpu.h:11` — `__per_cpu_offset` declaration

---

## Layer 1: Variable Declaration

### `DEFINE_PER_CPU(type, name)`

```c
/* include/linux/percpu-defs.h:114 */
#define DEFINE_PER_CPU(type, name)                  \
    DEFINE_PER_CPU_SECTION(type, name, "")

/* include/linux/percpu-defs.h:90 */
#define DEFINE_PER_CPU_SECTION(type, name, sec)     \
    __PCPU_ATTRS(sec) __typeof__(type) name

/* include/linux/percpu-defs.h:49 */
#define __PCPU_ATTRS(sec)                           \
    __percpu __attribute__((section(PER_CPU_BASE_SECTION sec)))

/* PER_CPU_BASE_SECTION = ".data..percpu" (SMP) or ".data" (UP) */
```

**Result:** `DEFINE_PER_CPU(int, my_counter)` expands to:
```c
__percpu __attribute__((section(".data..percpu"))) int my_counter;
```

The variable `my_counter` now lives in the `.data..percpu` ELF section.

### Specialized Variants

```c
/* Page-aligned per-CPU (separate page avoids false sharing) */
DEFINE_PER_CPU_PAGE_ALIGNED(struct per_cpu_pageset, boot_pageset)
→ section ".data..percpu..page_aligned"

/* Read-mostly (placed in dedicated cache lines) */
DEFINE_PER_CPU_READ_MOSTLY(struct task_struct *, current_task)
→ section ".data..percpu..read_mostly"

/* First in section (cpu_number, etc.) */
DEFINE_PER_CPU_FIRST(int, cpu_number)
→ section ".data..percpu..first"

/* Module per-CPU (allocated from reserved region, not static section) */
/* Used in loadable kernel modules: */
/* MODULE_PER_CPU(type, name) */
```

### Declaring Without Defining

```c
/* In header files: */
DECLARE_PER_CPU(int, my_counter);   /* extern declaration */

/* In source files: */
DEFINE_PER_CPU(int, my_counter);    /* actual definition */
```

---

## Layer 2: `SHIFT_PERCPU_PTR` — The Core Address Computation

```c
/* include/linux/percpu-defs.h:230 */
#define SHIFT_PERCPU_PTR(__p, __offset)                         \
    RELOC_HIDE((typeof(*(__p)) __kernel __force *)(__p), (__offset))

/* include/linux/compiler.h: */
#define RELOC_HIDE(ptr, off)                                    \
    ({                                                          \
        unsigned long __ptr;                                    \
        __asm__("" : "=r"(__ptr) : "0"(ptr));                  \
        (typeof(ptr)) (__ptr + (off));                          \
    })
```

### What `RELOC_HIDE` Does

The inline `asm("" : "=r"(__ptr) : "0"(ptr))` is a **barrier trick**:
1. It forces `ptr` into a register (`"0"(ptr)` = same register as output)
2. The output `"=r"(__ptr)` reads it back
3. The empty asm body does nothing

**Purpose:** The compiler sees this as a non-trivial operation. It cannot:
- Prove that `__ptr` equals `ptr` (register copy through inline asm)
- Perform type-based aliasing optimizations that would reorder the load
- Hoist the access above any preceding stores

The result: `(typeof(ptr))(__ptr + off)` — the pointer shifted by the per-CPU offset.

---

## Layer 3: `per_cpu_ptr()` — Cross-CPU Pointer

```c
/* include/linux/percpu-defs.h:233 */
#define per_cpu_ptr(ptr, cpu)               \
    ({                                      \
        __verify_pcpu_ptr(ptr);             \
        SHIFT_PERCPU_PTR((ptr),             \
                         per_cpu_offset(cpu)); \
    })

/* per_cpu_offset(cpu) = __per_cpu_offset[cpu]     */
/* (from include/asm-generic/percpu.h:21)           */
```

Usage:
```c
int *cpu2_counter = per_cpu_ptr(&my_counter, 2);
/* cpu2_counter = &my_counter + __per_cpu_offset[2] */
/* Points to CPU 2's private copy of my_counter      */

/* Then: */
*cpu2_counter = 42;  /* modifies CPU 2's my_counter */
```

**Important:** `per_cpu_ptr()` does NOT need preemption disabled. It accesses
**another CPU's** data by explicit CPU index — no reliance on the current CPU.

---

## Layer 4: `this_cpu_ptr()` — Current CPU Pointer

```c
/* include/linux/percpu-defs.h:250 */
#ifndef CONFIG_DEBUG_PREEMPT
#define this_cpu_ptr(ptr)  raw_cpu_ptr(ptr)
#else
#define this_cpu_ptr(ptr)  \
    ({                                                  \
        __verify_pcpu_ptr(ptr);                         \
        SHIFT_PERCPU_PTR(ptr, __my_cpu_offset);         \
    })
#endif

/* raw_cpu_ptr: */
#define raw_cpu_ptr(ptr)                \
    ({                                  \
        __verify_pcpu_ptr(ptr);         \
        arch_raw_cpu_ptr(ptr);          \
    })

/* arch_raw_cpu_ptr on ARM/ARM64: */
/* = SHIFT_PERCPU_PTR(ptr, __my_cpu_offset) */
/* where __my_cpu_offset reads TPIDRPRW/tpidr_el1 */
```

**Critical:** `this_cpu_ptr()` is **only valid** when the current CPU cannot change:
- In interrupt context (interrupts disabled = no preemption)
- In atomic context
- Between `preempt_disable()` and `preempt_enable()`

If called with preemption enabled and the thread migrates between CPUs, the pointer
would point to the wrong CPU's data.

---

## Layer 5: `per_cpu()` — Convenience Variable Access

```c
/* include/linux/percpu-defs.h:269 */
#define per_cpu(var, cpu)  (*per_cpu_ptr(&(var), cpu))

/* This is just a dereference of the pointer */
/* Equivalent to: *(type*)(&var + __per_cpu_offset[cpu]) */
```

---

## The `this_cpu_*` and `get_cpu_var()` API

### Read/Write Operations

```c
/* Read */
this_cpu_read(var)          /* no preempt disable (caller's responsibility) */
get_cpu_var(var)            /* per-cpu dereference WITH preempt_disable()   */
__this_cpu_read(var)        /* interrupt-safe (no preempt needed in irq ctx) */
raw_cpu_read(var)           /* no preemption, no debugging */

/* Write */
this_cpu_write(var, val)    /* no preempt disable */
get_cpu_var(var) = val;     /* ... then put_cpu_var(var) */

/* Arithmetic (RMW) */
this_cpu_add(var, val)      /* add to current CPU's var */
this_cpu_inc(var)           /* increment */
this_cpu_dec(var)           /* decrement */
this_cpu_or(var, val)       /* bitwise OR */
this_cpu_and(var, val)      /* bitwise AND */
```

### `get_cpu_var()` — Safe Full Access

```c
#define get_cpu_var(var)            \
    ({                              \
        preempt_disable();          \
        this_cpu_var(var);          \
    })

#define put_cpu_var(var)            \
    ({                              \
        (void)&(var);               \
        preempt_enable();           \
    })

/* Usage pattern: */
struct my_struct *p = &get_cpu_var(my_percpu_struct);
p->field = value;
put_cpu_var(my_percpu_struct);
```

---

## Expansion Example: Full Chain

Tracing `this_cpu_read(my_counter)` on ARM64:

```
this_cpu_read(my_counter)
  ↓
raw_cpu_read(my_counter)           [percpu-defs.h:250]
  ↓
*arch_raw_cpu_ptr(&my_counter)     [arch/arm64: SHIFT_PERCPU_PTR + msr access]
  ↓
*SHIFT_PERCPU_PTR(&my_counter, __my_cpu_offset)
  ↓
*SHIFT_PERCPU_PTR(&my_counter, __kern_my_cpu_offset())
  ↓
*RELOC_HIDE(&my_counter, (mrs tpidr_el1))
  ↓
*(int *)((unsigned long)&my_counter + tpidr_el1_value)
```

Assembly output:
```asm
mrs   x0, tpidr_el1        ; read __per_cpu_offset[current_cpu]
ldr   w1, [x0, #<my_counter_offset>]  ; load from CPU's private copy
; (compiler merges the RELOC_HIDE add with the load address calculation)
```

---

## `__verify_pcpu_ptr()` — Compile-Time Safety Check

```c
/* include/linux/percpu-defs.h:213 */
#define __verify_pcpu_ptr(ptr)                          \
do {                                                    \
    const void __percpu *__vpp_verify = (typeof((ptr) + 0))NULL; \
    (void)__vpp_verify;                                 \
} while (0)
```

This uses `__percpu` sparse annotation. If you accidentally pass a non-percpu pointer
to `per_cpu_ptr()`, `sparse` (the kernel static analysis tool) will warn:
```
warning: incorrect type in argument ... (different address spaces)
```

This prevents bugs like accidentally computing a bogus address by passing a regular
pointer to a per-CPU macro.

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Where are per-CPU variables placed? | `.data..percpu` ELF section |
| What linker symbols bound this section? | `__per_cpu_start`, `__per_cpu_end` |
| What does `SHIFT_PERCPU_PTR` do? | Adds per-CPU offset to a template pointer |
| What does `RELOC_HIDE` do? | Prevents compiler from aliasing-analyzing the pointer addition |
| Difference between per_cpu_ptr and this_cpu_ptr? | per_cpu_ptr: explicit CPU index; this_cpu_ptr: current CPU (needs preempt disabled) |
| What is `get_cpu_var()`? | Calls preempt_disable() then returns this_cpu_ptr() reference |
| What is `__percpu` annotation? | Sparse type tag for per-CPU pointers — caught by static analysis |
| Can per_cpu_ptr race with migration? | No — uses explicit CPU index, not current CPU register |
