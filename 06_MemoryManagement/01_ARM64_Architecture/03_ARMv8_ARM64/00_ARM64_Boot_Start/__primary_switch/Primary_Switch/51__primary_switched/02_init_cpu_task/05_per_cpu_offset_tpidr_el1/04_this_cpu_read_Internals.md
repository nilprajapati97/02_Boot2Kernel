# `this_cpu_read` / `this_cpu_write` — How Per-CPU Access Works

## The C Interface

```c
// Reading a per-CPU variable:
int val = this_cpu_read(my_counter);

// Writing:
this_cpu_write(my_counter, 42);

// Arithmetic:
this_cpu_inc(my_counter);
this_cpu_add(my_counter, 5);
```

These macros are defined in `include/linux/percpu-defs.h` and expand to
architecture-specific inline assembly.

---

## ARM64 Expansion of `this_cpu_read`

```c
// include/asm-generic/percpu.h (simplified ARM64 version):

#define this_cpu_read(pcp)                                          \
({                                                                  \
    typeof(pcp) __retval;                                           \
    __retval = raw_cpu_read(pcp);                                   \
    __retval;                                                       \
})

#define raw_cpu_read(pcp)                                           \
({                                                                  \
    unsigned long __cpu_offset;                                     \
    __cpu_offset = get_this_cpu_offset();  /* mrs tpidr_el1 */      \
    *per_cpu_ptr(&(pcp), __cpu_offset);    /* load from offset+va */\
})
```

At the assembly level, for `this_cpu_read(my_counter)`:
```asm
mrs     x0, tpidr_el1                    // x0 = per-CPU offset for this CPU
ldr     w1, [x0, #OFFSET_my_counter]     // w1 = my_counter value for this CPU
```

TWO instructions. The first gives the per-CPU base; the second does the variable access.

---

## `per_cpu_ptr` — Converting a Symbol to a Per-CPU Pointer

```c
// include/linux/percpu-defs.h:

#define per_cpu_ptr(ptr, cpu)                                       \
({                                                                  \
    __verify_pcpu_ptr(ptr);                                         \
    (typeof(*(ptr)) __kernel __force *)                             \
        ((unsigned long)(ptr) + per_cpu_offset(cpu));               \
})

#define per_cpu_offset(cpu)  (__per_cpu_offset[cpu])
```

So `per_cpu_ptr(&my_counter, 0)`:
```c
= (int *)((unsigned long)&my_counter + __per_cpu_offset[0])
= (int *)(prototype_va + __per_cpu_offset[0])
= (int *)(cpu0_section_va + OFFSET_my_counter)
```

---

## `get_this_cpu_offset` — The Fast Path

```c
// arch/arm64/include/asm/percpu.h:

static __always_inline unsigned long get_this_cpu_offset(void)
{
    unsigned long off;
    asm("mrs\t%0, " __stringify(tpidr_el1) : "=r" (off));
    return off;
}

static __always_inline void set_this_cpu_offset(unsigned long off)
{
    asm("msr\t" __stringify(tpidr_el1) ", %0" :: "r" (off));
}
```

`init_cpu_task` calls `set_this_cpu_offset` (the assembly macro version) to
initialize `tpidr_el1`. After this, every `this_cpu_read/write` works correctly
for the boot CPU.

---

## Performance: Per-CPU vs Global Variable

Scenario: A scheduler needs to read the current CPU's runqueue.

**Global array approach (WRONG for performance):**
```c
struct rq runqueues[NR_CPUS];
// Access: requires knowing cpu id first
int cpu = smp_processor_id();       // involves atomic read or register read
struct rq *rq = &runqueues[cpu];    // index computation
```

**Per-CPU approach (CORRECT):**
```c
DEFINE_PER_CPU(struct rq, runqueues);
// Access:
struct rq *rq = this_cpu_ptr(&runqueues);
// Expands to:
// mrs x0, tpidr_el1          (1 instruction: per-CPU offset)
// add x1, x0, #OFFSET_rq     (1 instruction: address of rq for this CPU)
```

**Cache behavior difference:**
- Global array: `runqueues[0]` and `runqueues[1]` may share a cache line if small.
  CPU1 writing `runqueues[1]` can cause false sharing on CPU0's cache line.
- Per-CPU: each CPU's `runqueues` is in a completely separate memory region
  (64KB apart). ZERO false sharing.

---

## `tpidr_el1` vs `tpidr_el0` — Why Two Thread Registers?

```
tpidr_el0 (Thread ID Register EL0):
    Used by user-space thread-local storage (TLS)
    Reading from EL0: user code does 'mrs x0, tpidr_el0' to get TLS pointer
    Writing from EL1: kernel sets tpidr_el0 on each context switch
    Value: &current->thread.tp_value (user TLS base)

tpidr_el1 (Thread ID Register EL1):
    Kernel-only register (user code cannot read it without fault)
    Set once per CPU at boot by init_cpu_task
    Value: __per_cpu_offset[cpu] = per-CPU base for this CPU
```

They serve completely different purposes:
- `tpidr_el0` = per-THREAD (changes on every context switch)
- `tpidr_el1` = per-CPU (set once at CPU bringup, only changes if per-CPU layout changes)

`init_cpu_task` sets `tpidr_el1`. Context switch code (`cpu_switch_to`) updates
`tpidr_el0` (to point to the new task's user TLS).

---

## What Breaks Without `tpidr_el1` Set

The very first C code that uses any per-CPU variable would crash. In `start_kernel`,
this happens within the first few function calls:

```c
void __init start_kernel(void) {
    set_task_stack_end_magic(&init_task);
    smp_setup_processor_id();       // calls smp_processor_id() → uses per-CPU
    // → this_cpu_read(cpu_number) → mrs tpidr_el1 → garbage → crash
}
```

`smp_processor_id()` is one of the earliest per-CPU accesses. Without `tpidr_el1`,
the kernel panics here with a translation fault accessing a garbage address.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
TPIDR_EL1 (Thread ID Register for EL1) is a 64-bit system register available at EL1 for software use. ARMv8-A defines it as "EL1 software thread ID register" -- it holds an arbitrary 64-bit value chosen by the OS. On SMP Linux, each CPU stores the address of its per-CPU offset (or a pointer to the CPU's data) in TPIDR_EL1. Reading TPIDR_EL1 is a single MRS instruction with no memory access, making it the fastest way to identify which CPU is executing.

### Kernel Perspective (Linux ARM64)
Linux uses TPIDR_EL1 to point to the per-CPU offset table. The THIS_CPU_READ/WRITE macros use:
  mrs x0, tpidr_el1     // load per-CPU base
  ldr x0, [x0, #offset] // load per-CPU variable
In __primary_switched, TPIDR_EL1 is initialized to the boot CPU's per-CPU offset via msr tpidr_el1, x27 (or equivalent). Per-CPU variables are declared with DEFINE_PER_CPU and accessed through get_cpu_var()/put_cpu_var(). The CPU does not need to disable preemption to read its own TPIDR_EL1 because the register is private to each CPU.

### Memory Perspective (ARMv8 Memory Model)
Per-CPU data is physically spread across NUMA nodes or simply across L1 cache regions. By using per-CPU data, Linux avoids cache line bouncing: each CPU has its own copy of the data in its own L1 D-cache, with no sharing or coherency traffic between CPUs. TPIDR_EL1 itself lives in the system register file (not RAM), so reading it has zero memory latency. The pointed-to per-CPU memory is Normal Inner-Shareable, but because each CPU only reads its own slice, there is no actual sharing -- the inner-shareable attribute just means the memory type is compatible with the cache coherency domain.