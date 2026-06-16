# Per-CPU Allocator: alloc_percpu and TPIDR_EL1

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Per-CPU variables: data that has separate instances per CPU core
  
  Why per-CPU?
    Eliminate locking: if each CPU has its own counter/list,
    no lock needed — CPU 0 reads CPU 0's data, CPU 1 reads CPU 1's data
    No cache contention: each CPU's data on different cache lines
    NUMA-friendly: per-CPU data on same NUMA node as the CPU
  
  Examples of per-CPU data in Linux:
    struct per_cpu_pages pcp:    per-CPU page cache (buddy allocator)
    struct runqueue rq:          scheduler run queue per CPU
    irq_stat:                    interrupt statistics per CPU
    net_device stats:            packet counters per CPU
    lockdep per-CPU structures
    Performance counters (perf)
    RCU (Read-Copy-Update) data
  
  ARM64 per-CPU mechanism:
    TPIDR_EL1: Thread Pointer Register for EL1 (kernel)
    Kernel stores: per-CPU offset (distance from reference point) in TPIDR_EL1
    
    When accessing per-CPU variable:
      1. Read TPIDR_EL1 (contains this CPU's offset)
      2. Add offset to variable's "per-CPU pointer" (fixed address)
      3. Access the per-CPU instance
    
    This is very fast:
      MRS X0, TPIDR_EL1         // 1 cycle: read CPU offset
      ADD X1, X_percpu_ptr, X0  // 1 cycle: add offset
      LDR/STR via X1            // access per-CPU data
    
    No lock, no atomic operation, no cache coherency overhead!
```

---

## 2. Static Per-CPU Variables

```c
/* include/linux/percpu-defs.h */

// Define a per-CPU variable:
DEFINE_PER_CPU(type, name);
// Equivalent to: __attribute__((section(".data..percpu"))) type name;
// Creates ONE instance in the .data..percpu section (used as template)

DEFINE_PER_CPU_ALIGNED(type, name);  // cache-line aligned (prevent false sharing)
DEFINE_PER_CPU_PAGE_ALIGNED(type, name);  // page aligned

// Examples:
DEFINE_PER_CPU(struct per_cpu_pages, boot_pageset);
DEFINE_PER_CPU(struct runqueue, runqueues);
DEFINE_PER_CPU_ALIGNED(struct irq_stack, irq_stacks);

// Access per-CPU variable (must be in non-preemptible context!):
per_cpu(var, cpu_id):      // access CPU cpu_id's instance
this_cpu_read(var):        // read current CPU's instance (optimized)
this_cpu_write(var, val):  // write current CPU's instance
this_cpu_add(var, val):    // add to current CPU's instance (atomic on some archs)
this_cpu_ptr(var):         // pointer to current CPU's instance

// Preemption-safe access:
cpu = get_cpu();           // disable preemption, get current CPU number
p = per_cpu_ptr(&var, cpu);
// ... access p ...
put_cpu();                 // re-enable preemption

// Why preemption matters:
// If preempted between get_cpu() and use:
//   Migrated to different CPU → reading WRONG CPU's data!
// Solution: disable preemption for duration of per-CPU access
```

---

## 3. Dynamic Per-CPU Allocation

```c
/* include/linux/percpu.h */

// Allocate per-CPU memory at runtime:
void __percpu *alloc_percpu(type)
// Macro: allocates sizeof(type) bytes for each CPU

void __percpu *alloc_percpu_gfp(type, gfp_t gfp)
// With explicit GFP flags

void __percpu *__alloc_percpu(size_t size, size_t align)
// Raw: explicit size and alignment

void free_percpu(void __percpu *ptr)

// Example: network driver per-CPU stats:
struct my_stats {
    u64 rx_packets;
    u64 tx_packets;
    u64 errors;
} __aligned(2 * sizeof(u64));

// In driver init:
priv->stats = alloc_percpu(struct my_stats);
if (!priv->stats) return -ENOMEM;

// In IRQ handler (non-preemptible):
struct my_stats *s = this_cpu_ptr(priv->stats);
s->rx_packets++;

// To sum all CPUs:
u64 total = 0;
for_each_possible_cpu(cpu)
    total += per_cpu_ptr(priv->stats, cpu)->rx_packets;

// Cleanup:
free_percpu(priv->stats);
```

---

## 4. ARM64 per-CPU Implementation Detail

```
ARM64 per-CPU setup:

1. At boot: pcpu_setup_first_chunk() [mm/percpu.c]:
   Allocates 'nr_cpus' chunks of memory
   Each chunk = pcpu_unit_size bytes (holds all per-CPU variables for 1 CPU)
   
   Physical layout (typical ARM64 server with 4 CPUs):
   [CPU0 chunk: 4MB][CPU1 chunk: 4MB][CPU2 chunk: 4MB][CPU3 chunk: 4MB]
   
   These chunks are at different virtual addresses

2. per_cpu_offset array:
   __per_cpu_offset[0] = (VA of CPU0 chunk) - (VA of .data..percpu section)
   __per_cpu_offset[1] = (VA of CPU1 chunk) - (VA of .data..percpu section)
   etc.
   
   The offset is the DELTA between the static template address and the actual per-CPU chunk

3. TPIDR_EL1 setup:
   In __per_cpu_thread_info_init() / early CPU setup:
   MSR TPIDR_EL1, Xn   // Xn = __per_cpu_offset[this_cpu]
   
   After SMP bring-up:
   cpu_switch_mm() or secondary_start_kernel():
   MSR TPIDR_EL1, Xn   // Each secondary CPU sets its own offset

4. per_cpu access on ARM64:
   // this_cpu_ptr(ptr) compiles to:
   MRS  X1, TPIDR_EL1           // X1 = __per_cpu_offset[current_cpu]
   ADD  X0, X_ptr, X1           // X0 = ptr + per_cpu_offset
   // Now X0 points to this CPU's instance
   LDR  W2, [X0]                // read per-CPU value
   
   Total: 3 instructions, no memory barriers, no locks
   This is why per-CPU is so efficient!

5. Per-CPU on context switch:
   When kernel switches tasks: TPIDR_EL1 does NOT change
   (TPIDR_EL1 tracks CPU, not task)
   TPIDR_EL0 = user-space thread pointer (__builtin_thread_pointer in glibc)
   TPIDR_EL1 = kernel per-CPU offset (set at CPU startup, never changes)
   
   Exception: cpu hotplug — when CPU comes online:
   cpu_up() → secondary_start_kernel() → 
     arch_setup_new_exec() / per_cpu_offset initialization:
       MSR TPIDR_EL1, X_new_offset

ARM64 per-CPU for KPTI (Kernel Page Table Isolation):
   When KPTI enabled (Meltdown mitigation):
   TPIDR_EL1: kernel per-CPU offset
   Each CPU has TWO page table sets: kernel + user
   Switching between EL0 and EL1 requires switching TTBR1
   Per-CPU TPIDR_EL1 used to find the trampoline/idmap page for the switch
```

---

## 5. Interview Questions & Answers

**Q1: Why must you disable preemption when accessing a per-CPU variable? What can go wrong?**

Preemption means the scheduler can interrupt the current task at (almost) any point and move it to a different CPU. Per-CPU variables are accessed using the CURRENT CPU's offset (from TPIDR_EL1). If:

1. Task reads per-CPU variable: `p = this_cpu_ptr(&stats)` → gets pointer to CPU0's data
2. Scheduler preempts and migrates task to CPU2
3. Task writes: `p->rx_packets++` → modifies CPU0's data while running on CPU2!

Two problems:
- **Data corruption**: CPU0 and CPU2 both potentially modifying CPU0's stats
- **Incorrect data**: the task thinks it's updating "its" CPU's stats, but actually updates a different CPU's stats

Solutions:
- `get_cpu()` / `put_cpu()`: wraps access with `preempt_disable()` / `preempt_enable()`
- Interrupt handlers: automatically non-preemptible (hardware IRQ disables preemption)
- `local_irq_save()`: fully serializes on current CPU

Note: `this_cpu_read()` / `this_cpu_write()` macros on ARM64 DON'T automatically disable preemption — the caller must ensure non-preemptible context. The `__this_cpu_*` variants (with double underscore) add no preemption check at all (for performance-critical paths where preemption is already disabled).

**Q2: What is TPIDR_EL0 used for on ARM64? How does it differ from TPIDR_EL1?**

- **TPIDR_EL1** (Thread Pointer, EL1): kernel use only. Stores per-CPU base offset. Set once during CPU initialization, never changed. Only accessible from EL1 (kernel). Used by: `this_cpu_ptr()`, per-CPU page sets, scheduler runqueues.

- **TPIDR_EL0** (Thread Pointer, EL0): accessible from EL0 (user). Stores the thread-local storage (TLS) base pointer for each user thread. Changed on every context switch (`thread_struct.uw.tp_value` → `MSR TPIDR_EL0`). Used by: glibc's `__thread` variables, `pthread_getspecific()`, `errno` (which is thread-local in glibc).

When a user program executes `mrs x0, tpidr_el0`, it reads its own TLS pointer. When the kernel runs `mrs x0, tpidr_el1`, it reads the per-CPU offset. These are completely independent registers with separate values.

On context switch (`__switch_to()`):
```
MSR TPIDR_EL0, Xn    // restore new task's TLS pointer
// TPIDR_EL1 unchanged: it's per-CPU, not per-task
```

---

## 6. Quick Reference

| Register | Users | Contents | Changed On |
|---|---|---|---|
| TPIDR_EL0 | User space | TLS pointer per thread | Context switch |
| TPIDR_EL1 | Kernel | Per-CPU base offset | CPU hotplug only |
| TPIDR_EL2 | Hypervisor | EL2 per-CPU data | VM switch |

| Per-CPU Access Method | Preemption Safe? | Use Case |
|---|---|---|
| this_cpu_ptr() | Must disable externally | General access |
| get_cpu() / put_cpu() | YES (disables preemption) | Portable safe access |
| In IRQ handler | YES (IRQ = non-preemptible) | Interrupt stats |
| per_cpu(var, n) | N/A (explicit CPU) | Cross-CPU read |
