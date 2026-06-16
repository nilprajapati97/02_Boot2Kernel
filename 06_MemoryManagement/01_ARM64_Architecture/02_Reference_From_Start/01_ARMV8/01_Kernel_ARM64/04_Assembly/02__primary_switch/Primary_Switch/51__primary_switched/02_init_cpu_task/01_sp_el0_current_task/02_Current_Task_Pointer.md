# `current` Task Pointer — Deep Technical Analysis

## What `current` Returns and How It's Used

```c
// Typical kernel C code:
struct task_struct *task = current;

// Accessing task fields:
pid_t my_pid   = current->pid;
uid_t my_uid   = current_uid().val;
char *name     = current->comm;  // process name string
```

`current` is NOT a global variable — it is a function-like macro that expands to
an `mrs sp_el0` instruction. Every access to `current` generates one `mrs` instruction
in the compiled output.

---

## `task_struct` vs `thread_info` — Historical Split

Historically, the kernel stored `thread_info` at the BOTTOM of the kernel stack:
```
[old approach - before CONFIG_THREAD_INFO_IN_TASK]

kernel stack memory:
  [bottom] thread_info { ... }
           ↓ stack grows up...
  [top]    most recent stack frame
```

The `current` macro was: `(struct task_struct *)((sp & ~(THREAD_SIZE-1)) + OFFSET)`.
One AND + one LOAD from memory.

**ARM64 modern approach (since ~Linux 4.9):**
```c
// CONFIG_THREAD_INFO_IN_TASK=y
// thread_info is INSIDE task_struct at offset 0
struct task_struct {
    struct thread_info  thread_info;  // ← at offset 0, embedded
    ...
};
```

Now `&task_struct == &thread_info` — same address. And `sp_el0 = &task_struct`.

The `current` macro is just `mrs sp_el0, x0` — one instruction, zero memory.

---

## `init_task` — The Boot Task

`init_task` (PID 0, "swapper") is the task that runs during boot:

```c
// kernel/init_task.c
struct task_struct init_task
__init_task_data = {
    .pid            = 0,
    .tgid           = 0,
    .state          = 0,       // TASK_RUNNING
    .stack          = init_stack,
    .comm           = INIT_TASK_COMM,  // "swapper/0"
    .active_mm      = &init_mm,
    .flags          = PF_KTHREAD | PF_NOFREEZE,
    .thread_info    = INIT_THREAD_INFO(init_task),
    ...
};
```

`__init_task_data` marks the variable as residing in `.data..init_task` linker
section — a specially aligned section that is NEVER freed (unlike `.init.data`).

`init_task` persists for the entire kernel lifetime. It becomes the idle task for
CPU0 after `start_kernel` forks off PID 1 (`init` process).

---

## `current` in Interrupt Context — Edge Case

When the kernel is executing an interrupt handler, `current` returns the task that
was **interrupted** — NOT the interrupt handler itself. This is because:

1. Interrupt entry saves the context of the preempted task
2. `sp_el0` is NOT changed during IRQ entry/exit
3. The IRQ handler runs on the IRQ stack (configured by `cpu_init`) but `sp_el0`
   still points to the interrupted task

```c
// An IRQ handler can safely call:
struct task_struct *interrupted_task = current;  // OK — who was running
if (in_interrupt()) {
    // We're in IRQ context
    // current = the task that was preempted
}
```

---

## What Happens When `sp_el0` Is Wrong

If `msr sp_el0, x4` had NOT been executed before the first C call:

```
SCENARIO: sp_el0 is 0x0 (e.g., firmware left it zero)

First call to current:
    mrs x0, sp_el0   → x0 = 0x0
    ; code does: current->pid
    ldr w1, [x0, #PID_OFFSET]  → load from address 0x0 + PID_OFFSET
    ; This is a NULL pointer dereference
    ; Translation fault at EL1
    ; VBAR_EL1 vectors invoked
    ; But VBAR_EL1 might also be zero/invalid!
    ; CPU fetches instruction from PA 0x0 + vector offset
    ; Boot ROM might be at PA 0x0 → executes firmware code
    ; Or nothing mapped → translation fault in fault handler
    ; Double fault → CPU resets
```

This is why `msr sp_el0, x4` is the **very first instruction** in `init_cpu_task`.

---

## KPTI and `sp_el0` — The Complication

With KPTI (Kernel Page-Table Isolation, aka "Meltdown mitigation"):
- User-space and kernel-space use DIFFERENT page tables (different TTBR0 values)
- When entering kernel mode, the kernel must switch TTBR0 and TTBR1

During the KPTI trampoline (before the main kernel VBAR handlers):
```asm
// KPTI user-kernel transition trampoline:
// At this point: sp_el0 might have been temporarily swapped with TTBR1!
// Linux uses a trick: save/restore sp_el0 in kernel_ventry
```

The KPTI entry code in `arch/arm64/kernel/entry.S` temporarily clobbers `sp_el0`
to swap TTBR values, then restores it. This is safe because the swap is atomic
from the point of view of any C code (it happens entirely in assembly).

`current` in C code always sees the correct `sp_el0 = &current_task`.

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