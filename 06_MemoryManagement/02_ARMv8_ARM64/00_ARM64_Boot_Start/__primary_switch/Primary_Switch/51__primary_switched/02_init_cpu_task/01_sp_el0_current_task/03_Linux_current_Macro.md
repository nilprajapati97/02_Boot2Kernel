# The Linux `current` Macro — From ARM64 Register to C Struct

## Header File Chain

```
arch/arm64/include/asm/current.h
    ↓ #include
include/linux/sched.h (uses 'current')
    ↓ used by
All kernel C files that use 'current'
```

---

## `current.h` Full Implementation

```c
// arch/arm64/include/asm/current.h

#ifndef __ASSEMBLY__

#include <linux/compiler.h>

struct task_struct;

static __always_inline struct task_struct *get_current(void)
{
    unsigned long sp_el0;

    asm ("mrs\t%0, sp_el0" : "=r" (sp_el0));

    return (struct task_struct *)sp_el0;
}

#define current get_current()

#endif /* __ASSEMBLY__ */
```

**Key annotations:**
- `__always_inline` — the function is ALWAYS inlined; never becomes a real function call
- `"mrs\t%0, sp_el0"` — inline assembly; `%0` is the output operand `sp_el0`
- `"=r"` constraint — output to any general-purpose register
- Cast to `struct task_struct *` — the register value IS the task pointer

---

## Compiled Output

For the C code:
```c
void show_current_pid(void) {
    printk("pid = %d\n", current->pid);
}
```

ARM64 assembly output (with `-O2`):
```asm
show_current_pid:
    stp     x29, x30, [sp, #-32]!   // frame setup
    mov     x29, sp
    mrs     x0, sp_el0              // x0 = current (ONE INSTRUCTION!)
    ldr     w1, [x0, #PID_OFFSET]   // load current->pid
    adrp    x0, fmt_string
    add     x0, x0, :lo12:fmt_string
    bl      printk
    ldp     x29, x30, [sp], #32
    ret
```

The `current` access compiles to a single `mrs x0, sp_el0` — no memory load,
no cache miss, no TLB lookup. Just a register read.

---

## Comparison with Other Architectures

| Architecture | `current` Implementation | Instructions | Memory Access? |
|---|---|---|---|
| ARM64 | `mrs sp_el0, xN` | 1 | No |
| x86-64 | `mov %gs:OFFSET, %rax` | 1 | Yes (but from GSBASE reg) |
| ARM32 | `(sp & ~(THREAD_SIZE-1))` | 2 (AND + load) | Sometimes |
| RISC-V | Per-CPU global | 2+ | Yes |
| MIPS | `k1` register (reserved) | 1 | No |

ARM64 and MIPS both use a dedicated register — the fastest possible approach.

---

## `current` Is Inlined Everywhere — Performance Impact

Because `current` is `__always_inline`, every call site generates one `mrs`
instruction. In a hot path like the scheduler or network stack, you might see:

```c
// Kernel code with multiple current accesses:
task_t *t = current;     // mrs x0, sp_el0
t->state = TASK_RUNNING; // str xN, [x0, #STATE_OFFSET]
wake_up(t->waitq);       // uses t = x0 (compiler keeps it in register)
```

The compiler is smart: after the first `mrs`, it keeps the value in a register
(`x0` or any other GPR). Subsequent uses of `current` in the same function
reuse the cached value — no repeated `mrs`. The `__always_inline` attribute
helps the optimizer see that `current` returns the same value throughout a
non-preemptible section.

---

## `get_task_struct` vs `current`

```c
// WRONG — if you need the task after blocking:
void some_work(void) {
    struct task_struct *t = current;  // mrs — ok
    schedule();                       // CPU switches to another task!
    // t still valid? t->pid could be wrong!
    // After schedule(), current might be a DIFFERENT task on this CPU
    do_something_with(t);   // DANGEROUS — t might have exited!
}

// CORRECT — increment reference count:
void some_work(void) {
    struct task_struct *t = get_task_struct(current);  // atomic_inc + mrs
    schedule();
    // t is still valid because refcount prevents it from being freed
    do_something_with(t);
    put_task_struct(t);     // decrement refcount
}
```

The lesson: `current` gives a POINTER, not a REFERENCE. After `schedule()` or
any blocking operation, `current` returns a potentially different task.

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