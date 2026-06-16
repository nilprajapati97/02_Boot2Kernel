# `sp_el0` as Current Task Pointer — The Core Concept

## The Problem: How Does the Kernel Know Its Own Task?

Every kernel C function can call `current` to get the running task:
```c
struct task_struct *t = current;
printk("pid=%d\n", t->pid);
```

On a uniprocessor, this could be a global pointer. On SMP with N CPUs, each CPU
needs to return a DIFFERENT pointer without any lock. The ARM64 solution is elegant:
use a hardware register that is **private to each CPU**.

---

## Why `sp_el0`?

ARM64 has multiple banked registers:
- `sp_el0` — EL0 (user) stack pointer
- `sp_el1` — EL1 (kernel) stack pointer (this IS the kernel's `sp`)
- `sp_el2` — EL2 (hypervisor) stack pointer
- `tpidr_el0`, `tpidr_el1`, `tpidr_el2` — thread ID registers
- `vbar_el1`, `vbar_el2` — vector base address

When running at EL1 (kernel mode), `sp_el0` is architecturally idle — the CPU
doesn't use it for anything automatically. Linux hijacks it.

**Why not `tpidr_el0`?** That's already used for the user-space thread pointer
(`pthread_self()`, TLS). Linux keeps `tpidr_el0` for user-space use.

**Why not `tpidr_el1`?** That's used for the per-CPU offset (`__per_cpu_offset[cpu]`).

`sp_el0` is the only free register that is:
- Hardware-banked (each CPU has its own copy)
- Not used by any standard ABI
- Accessible in a single instruction (`mrs`/`msr`)

---

## The Single Instruction to Set It

```asm
// __primary_switched (head.S)
adr_l   x4, init_task     // x4 = virtual address of init_task struct
msr     sp_el0, x4        // sp_el0 = &init_task
```

`msr sp_el0, x4` is a **system register write**:
- Privilege: Can only execute at EL1 or higher (not accessible from EL0/user-space)
- Latency: ~1-3 cycles (register-to-register, no memory access)
- Visibility: Immediate for subsequent `mrs sp_el0, xN` instructions (with ISB)

No ISB is needed after this `msr` in this context because the next use of `sp_el0`
is in `scs_load_current` which is several instructions later (enough pipeline
stages to observe the write).

---

## The Single Instruction to Read It — `current` Macro

```c
// arch/arm64/include/asm/current.h
static __always_inline struct task_struct *get_current(void)
{
    unsigned long sp_el0;
    asm ("mrs %0, sp_el0" : "=r" (sp_el0));
    return (struct task_struct *)sp_el0;
}
#define current get_current()
```

Compiled to one instruction:
```asm
mrs  x0, sp_el0   // x0 = current task pointer
```

**Zero memory accesses, zero cache lines, zero TLB entries.** It's just a
register read. This is the most efficient possible implementation of `current`
on any architecture.

---

## Context Switch — Maintaining `sp_el0`

When the scheduler switches from task A to task B on a CPU:
```c
// arch/arm64/kernel/process.c
__notrace_funcgraph static struct task_struct *
__switch_to(struct task_struct *prev, struct task_struct *next)
{
    ...
    cpu_switch_to(prev, next);  // assembly in head.S
    ...
}
```

```asm
// cpu_switch_to:
cpu_switch_to:
    ...
    msr     sp_el0, x1    // x1 = next task_struct; update sp_el0 to new task
    ...
    ret
```

When CPU switches to task B, `sp_el0` is updated to `&task_B`. Now `current`
returns task B's pointer. This happens in the **scheduler's critical section**,
so there's no window where `current` would return a wrong value.

---

## Security Implication — Why `sp_el0` Is Safe

A user-space program running at EL0 CANNOT read the kernel's `sp_el0`:
- `mrs sp_el0, x0` at EL0 reads the USER's own `sp_el0` (their stack pointer)
- It does NOT see the kernel's banked `sp_el0`

ARM64 register banking means: when at EL0, `sp_el0` is the user stack pointer.
When at EL1, `sp_el0` is the kernel's repurposed current-task register. They are
physically different registers accessed by the same name based on privilege level.

This prevents user-space from reading `&current` (which would be an information
leak about kernel ASLR). The architecture enforces this — no kernel code needed.

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