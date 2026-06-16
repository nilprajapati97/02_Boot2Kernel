# `sp_el0` / `current` Performance Analysis

## The One-Instruction Access Model

Every `current` access in C code compiles to:
```asm
mrs xN, sp_el0    // 1 instruction, 0 memory accesses
```

**Cortex-A78 latency** (approximate): 2-3 cycles for system register read.
Compare with:
- L1 data cache hit: 4 cycles
- L2 cache hit: 12 cycles
- L3 cache hit: 40 cycles
- DRAM access: 200+ cycles

The `mrs sp_el0` approach is ~2-10x faster than any memory-based alternative.

---

## Hot Path Analysis — Scheduler Overhead

The Linux scheduler (`kernel/sched/core.c`) calls `current` hundreds of times per
second per CPU. On a 4GHz CPU with 1000 context switches/second per CPU:

```
Context switches per second: 1,000
current accesses per switch (est): 20
Total mrs instructions/second: 20,000
Time per mrs: 2 cycles @ 4GHz = 0.5ns
Total time for current access/second: 20,000 × 0.5ns = 10,000ns = 10μs
```

10 microseconds per second = 0.001% CPU overhead. This is negligible.

If `current` used a memory load (L1 hit, 4 cycles):
```
20,000 × 1ns = 20μs/second = 0.002% CPU overhead
```

Still negligible — BUT this analysis ignores cache pressure. The `mrs` approach
uses ZERO cache lines. A memory-based `current` would occupy at least one cache
line per CPU, competing with other kernel data for L1 cache space.

---

## Critical Sections — `current` in Interrupt Handlers

In interrupt handlers, performance matters more than in background tasks:

```c
// Network receive handler (called on every packet):
static void nic_rx_handler(struct net_device *dev, ...)
{
    struct sock *sk = current->socket_list;  // mrs = 2 cycles
    // ... process packet
}
```

A network card at 10Gbps with 64-byte packets generates:
```
10Gbps / (64 bytes * 8 bits) = 19.5 million interrupts/second
```

At 19.5M interrupts/second, saving 2 cycles per `current` access vs. 4 cycles
saves 39M cycles/second ≈ ~10ms/second at 4GHz. That's significant for a
high-throughput network stack.

---

## `__always_inline` Impact on Code Generation

Because `get_current()` is `__always_inline`:

```c
// This C code:
void foo(void) {
    int pid1 = current->pid;
    // ... 100 lines of non-blocking code ...
    int pid2 = current->pid;  // second access
}
```

Compiled output:
```asm
foo:
    mrs     x0, sp_el0           // ONE mrs for BOTH accesses
    ldr     w1, [x0, #PID_OFF]   // pid1
    // ... 100 lines of code using x0 as cached 'current'...
    ldr     w2, [x0, #PID_OFF]   // pid2 — reuses x0 register
    ret
```

The compiler sees both `current` accesses, CSE (common subexpression elimination)
merges them into ONE `mrs` because `current` is a pure function (no side effects).
The second access is free.

This would NOT work if `current` were a global variable load — the compiler cannot
CSE across arbitrary code because some code might change the global.

---

## SMP Correctness — Why `sp_el0` Can't Be "Stale"

Concern: Could CPU-A's `sp_el0` ever reflect CPU-B's current task?

Answer: NO. `sp_el0` is a physical register inside the CPU core. Each CPU core
has its OWN physical `sp_el0` register. There is no shared memory involved.

When CPU-A migrates task T from CPU-B to CPU-A:
1. Task T is dequeued from CPU-B's runqueue (CPU-B stops running T)
2. Task T is enqueued to CPU-A's runqueue
3. CPU-A's scheduler picks T and calls `__switch_to`
4. `__switch_to` executes `msr sp_el0, xN` where xN = &T
5. CPU-A's physical `sp_el0` register now holds &T

CPU-B's `sp_el0` is unaffected — it still holds whatever task CPU-B is running now.
The physical isolation of CPU registers guarantees correctness without any locks.

---

## Profiling `current` Access — Tools

To measure `current` access overhead in a production kernel:

```bash
# perf stat counting system register reads (approximate):
perf stat -e r0014 -a sleep 1   # ARM_PMU_EVENT for system register access

# Or use ARM SPE (Statistical Profiling Extension):
perf record -e arm_spe//u -p <pid> -- <workload>
perf report --stdio

# ftrace: can't directly trace mrs instructions, but can time hot paths:
trace-cmd record -e sched:sched_switch -a sleep 1
```

In practice, the `current` access via `mrs sp_el0` is so fast it doesn't appear
in profiling output — it's below the profiler's sampling granularity.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the stack pointer is a dedicated register (SP_EL1 at EL1, SP_EL0 at EL0). SP_EL1 is the stack pointer used by the kernel during normal execution. The AAPCS64 ABI requires the stack to be 16-byte aligned at any instruction that may cause an exception. SCTLR_EL1.SA (bit 3) enables hardware enforcement of this alignment: if SP_EL1 is not 16-byte aligned when a load/store using SP is executed, an SP alignment fault is raised. The frame pointer (x29) is a general-purpose register used by convention to hold the base of the current stack frame. Writing x29 is the first act of any C function that wishes to be unwound.

### Kernel Perspective (Linux ARM64)
After the MMU is enabled, __primary_switch reinitializes the stack pointer to a virtual address. The early boot stack is defined as:
  __INIT_DATA: init_thread_union (size THREAD_SIZE, typically 16 KB)
The LDR instruction loads the VA of init_thread_union + THREAD_SIZE into x0, then MOV sp, x0 sets SP_EL1. This is necessary because the old stack pointer was set to a physical address (before the MMU) and that PA is no longer the correct address for the kernel VA layout. x29 is set to zero (zero frame pointer) to terminate the unwind chain at the first kernel stack frame.

### Memory Perspective (ARMv8 Memory Model)
The kernel stack resides in Normal Inner-Shareable Write-Back Cacheable memory (MT_NORMAL). Once the MMU and D-cache are enabled, all stack accesses (PUSH/POP equivalents: STP/LDP) go through the L1 D-cache. The L1 D-cache write-back policy means that the stack contents are not immediately visible to physical memory until a cache clean or eviction. This is safe for the stack because the kernel does not use DMA to read stack memory. The stack pointer reinitalization at VA is a hard cut: all future kernel stack frames exist in the high VA kernel mapping.