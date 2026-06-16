# `__per_cpu_offset` Array — The Per-CPU Offset Table

## What the Array Looks Like in Memory

```c
// include/linux/percpu.h
extern unsigned long __per_cpu_offset[NR_CPUS];
```

`__per_cpu_offset` is a global array of `unsigned long` (64-bit on ARM64).
Each element gives the byte offset from the per-CPU section PROTOTYPE to the
per-CPU section COPY for that CPU.

```
Memory layout (4-CPU system, per-CPU section size = 64KB):

__per_cpu_offset[0] = 0x0000_0000_0010_0000   // CPU0 copy at prototype + 1MB
__per_cpu_offset[1] = 0x0000_0000_0011_0000   // CPU1 copy at prototype + 1MB+64KB
__per_cpu_offset[2] = 0x0000_0000_0012_0000   // CPU2 copy at prototype + 1MB+128KB
__per_cpu_offset[3] = 0x0000_0000_0013_0000   // CPU3 copy at prototype + 1MB+192KB
```

---

## How Per-CPU Variables Are Defined

```c
// Define a per-CPU variable:
DEFINE_PER_CPU(int, my_counter);

// This expands to:
int my_counter __attribute__((section(".data..percpu")));
```

The variable `my_counter` is placed in the `.data..percpu` section at a fixed
offset from the section start. Call this offset `OFFSET_my_counter`.

**CPU0 accesses its copy:**
```c
// this_cpu_read(my_counter) compiles to roughly:
// VA = __per_cpu_offset[0] + OFFSET_my_counter
int val = *(int *)(__per_cpu_offset[0] + &my_counter);
```

But this is implemented via `tpidr_el1`:
```asm
mrs     x0, tpidr_el1                    // x0 = __per_cpu_offset[cpu]
ldr     w1, [x0, #OFFSET_my_counter]     // w1 = my_counter for this CPU
```

One `mrs` + one `ldr` = two instructions.

---

## The Per-CPU Section Layout in Virtual Memory

```
Virtual Address Space (ARM64 kernel, example):
──────────────────────────────────────────────

0xFFFF800010000000   .data..percpu PROTOTYPE (the original "template" copy)
                     [all per-CPU variables at their initial values]
                     [NR_CPUS × size reserved here... NO — prototype is ONE copy]

0xFFFF800010010000   CPU0 per-CPU section (copy of prototype, used at runtime)
                     my_counter for CPU0 at: 0xFFFF800010010000 + OFFSET_my_counter
                     runqueues[0] at:        0xFFFF800010010000 + OFFSET_runqueues

0xFFFF800010020000   CPU1 per-CPU section
                     my_counter for CPU1 at: 0xFFFF800010020000 + OFFSET_my_counter

0xFFFF800010030000   CPU2 per-CPU section

0xFFFF800010040000   CPU3 per-CPU section
```

`__per_cpu_offset[N]` = VA_of_CPU_N_section - VA_of_prototype_section

---

## Who Initializes `__per_cpu_offset`?

For the PRIMARY CPU (CPU0):

```c
// arch/arm64/mm/init.c or kernel/percpu.c
void __init setup_per_cpu_areas(void)
{
    // Allocate per-CPU sections for all CPUs
    for (int cpu = 0; cpu < nr_cpu_ids; cpu++) {
        void *ptr = alloc_percpu_section();
        // Copy prototype section to each CPU's section
        memcpy(ptr, __per_cpu_start, __per_cpu_end - __per_cpu_start);
        // Record offset
        __per_cpu_offset[cpu] = (unsigned long)ptr - (unsigned long)__per_cpu_start;
    }
}
```

**BUT** — `init_cpu_task` runs BEFORE `setup_per_cpu_areas()` (which is called
from `start_kernel`). How does `__per_cpu_offset[0]` have a valid value at boot?

The boot-time per-CPU section for CPU0 IS the prototype section itself:
```c
// At link time, the per-CPU section is placed at a fixed VA.
// __per_cpu_offset[0] is pre-initialized at compile time to the
// offset from the link-time per-CPU section base.
// For CPU0, this is typically 0 or the prototype section base offset.
```

The key: early in boot, CPU0 uses the PROTOTYPE section directly. `__per_cpu_offset[0]`
at that point contains the correct VA to access CPU0's per-CPU data.
`setup_per_cpu_areas()` later reorganizes and reallocates, then calls
`__this_cpu_write(offset, newval)` to update `tpidr_el1` and `__per_cpu_offset`.

---

## The `lsl #3` Trick — Why 8-Byte Stride

```asm
ldr     \tmp1, [\tmp1, \tmp2, lsl #3]
```

`__per_cpu_offset` is `unsigned long[NR_CPUS]` where `unsigned long` = 8 bytes
on ARM64 (LP64 model). So:
- `&__per_cpu_offset[0]` = array base
- `&__per_cpu_offset[1]` = array base + 8
- `&__per_cpu_offset[cpu]` = array base + cpu * 8

ARM64's `lsl #3` in a load offset shifts `tmp2` (CPU ID) left by 3 bits, which
multiplies by 8. This computes the byte offset in one instruction:

```
ldr x5, [x5, x6, lsl #3]
= load 64-bit value at address: x5 + (x6 << 3)
= load 64-bit value at address: &__per_cpu_offset[0] + cpu_id * 8
= __per_cpu_offset[cpu_id]
```

This is an **indexed register addressing mode** — all ARM64 CPUs support it
natively. Zero extra instructions needed for the multiply.

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