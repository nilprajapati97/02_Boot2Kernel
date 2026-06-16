# Per-CPU Data Architecture — The Foundation

## The Problem: SMP Without Locks

On an SMP system with N CPUs, many kernel subsystems need per-CPU private data:
- Runqueue (one per CPU for the scheduler)
- IRQ statistics (one per CPU)
- Interrupt controller state (one per CPU)
- Memory allocator caches (one per CPU, `kmem_cache_cpu`)
- Network packet queues (one per CPU)
- Performance counters (one per CPU)

Without per-CPU data, accessing these would require a lock. With per-CPU data,
each CPU accesses its own private copy — **zero lock, zero contention**.

---

## The Per-CPU Memory Model

At compile time, the linker creates a **prototype** per-CPU section:

```
Kernel image (.data..percpu section):
┌───────────────────────────────────┐  ← __per_cpu_start
│  runqueues  (prototype copy)      │  +0x000 (offset of runqueues in section)
│  irq_stat   (prototype copy)      │  +0x100
│  kmem_cache (prototype copy)      │  +0x200
│  ...                              │
└───────────────────────────────────┘  ← __per_cpu_end
```

At runtime (`setup_per_cpu_areas`), for each CPU N, the kernel allocates a COPY
of this entire section. The copy is placed at a new virtual address.

```
Per-CPU copies at runtime:
CPU0 copy: VA = 0xFFFF800010010000  (runqueues at +0x000, irq_stat at +0x100, ...)
CPU1 copy: VA = 0xFFFF800010020000
CPU2 copy: VA = 0xFFFF800010030000
...
```

---

## `__per_cpu_offset[cpu]` — The Base Address Array

```c
// include/linux/percpu.h (simplified):
extern unsigned long __per_cpu_offset[NR_CPUS];
```

`__per_cpu_offset[N]` = (VA of CPU N's per-CPU section copy) - (VA of prototype)

This is NOT an absolute address — it's a **relative offset** from the prototype
to the per-CPU copy. The linker generates per-CPU variable references as offsets
from `__per_cpu_start`. At runtime, adding `__per_cpu_offset[cpu]` to the
prototype offset gives the CPU-specific address.

**Example:**
```
Prototype of runqueues: at VA 0xFFFF800010000100 (in .data..percpu)
Prototype base:          VA 0xFFFF800010000000 = __per_cpu_start
CPU0 copy base:          VA 0xFFFF800010010000
__per_cpu_offset[0]    = 0xFFFF800010010000 - 0xFFFF800010000000 = 0x10000

To access runqueues on CPU0:
    VA = 0xFFFF800010000100 + __per_cpu_offset[0]
       = 0xFFFF800010000100 + 0x10000
       = 0xFFFF800010010100  ← CPU0's private runqueues
```

---

## `tpidr_el1` as Per-CPU Base Register

`tpidr_el1` is the ARM64 Thread ID Register for EL1. Linux repurposes it to
hold `__per_cpu_offset[cpu]` — the per-CPU offset for the currently running CPU.

```
tpidr_el1 = __per_cpu_offset[current_cpu]
```

Since each physical CPU has its OWN `tpidr_el1` register (it's a CPU register,
not memory), each CPU automatically has its own per-CPU offset. No locking needed.

---

## `this_cpu_read()` — The Fast Path

```c
// arch/arm64/include/asm/percpu.h
#define __my_cpu_offset  read_sysreg(tpidr_el1)

#define this_cpu_read(var)  \
    __pcpu_size_call_return(this_cpu_read_, var)

// Expands to (for 64-bit var):
static inline u64 this_cpu_read_8(const void *ptr)
{
    u64 val;
    asm("mrs x0, tpidr_el1\n"    // x0 = __per_cpu_offset[cpu]
        "ldr %0, [x0, %1]"       // val = *(x0 + offset_of_var)
        : "=r"(val)
        : "i"(/* linker offset of var */));
    return val;
}
```

Total cost: 2 instructions (`mrs` + `ldr`) + 1 L1 cache access.

---

## The 4-Instruction Sequence in `init_cpu_task`

```asm
// OP 5a: Get VA of __per_cpu_offset array
adr_l   \tmp1, __per_cpu_offset

// OP 5b: Load CPU ID from task_struct.thread_info.cpu
ldr     w\tmp2, [\tsk, #TSK_TI_CPU]   // zero-extends to 64-bit x\tmp2

// OP 5c: Load __per_cpu_offset[cpu_id] (8-byte stride via lsl #3)
ldr     \tmp1, [\tmp1, \tmp2, lsl #3]  // tmp1 = __per_cpu_offset[cpu_id]

// OP 5d: Install into tpidr_el1 (or tpidr_el2 under VHE)
set_this_cpu_offset \tmp1
```

For the boot CPU (CPU0): `cpu_id = 0`, so this loads `__per_cpu_offset[0]`.

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