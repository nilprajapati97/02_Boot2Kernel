# `x18` Register — Reserved for Shadow Call Stack

## ARM64 Register Allocation Table (Linux Kernel)

| Register | AAPCS64 Role | Linux Kernel Override | Reason |
|---|---|---|---|
| x0-x7 | Function args / return values | Same | Standard ABI |
| x8 | Indirect result location | Same | Standard ABI |
| x9-x15 | Caller-saved (scratch) | Same | Standard ABI |
| x16-x17 | Intra-procedure-call | Same | Used by linker stubs |
| **x18** | **Caller-saved (platform register)** | **SCS pointer** | **RESERVED** |
| x19-x28 | Callee-saved | Same | Standard ABI |
| x29 | Frame pointer | Same | Stack unwinder |
| x30 | Link register (LR) | Same | Return address |

**The key override:** `x18` is technically "caller-saved" in AAPCS64 (functions can use
it freely). Linux RESERVES it by compiling the entire kernel with `-ffixed-x18`.

---

## Compiler Flag: `-ffixed-x18`

```makefile
# arch/arm64/Makefile:
ifeq ($(CONFIG_SHADOW_CALL_STACK), y)
    KBUILD_CFLAGS += -ffixed-x18
    KBUILD_CFLAGS += -fsanitize=shadow-call-stack
endif
```

`-ffixed-x18` tells GCC/Clang:
- NEVER use `x18` as a general-purpose register for any variable
- NEVER save/restore `x18` in function prologues/epilogues
- Treat `x18` as if it doesn't exist for register allocation

`-fsanitize=shadow-call-stack` tells the compiler:
- Insert `str x30, [x18], #8` at every function entry
- Insert `ldr x30, [x18, #-8]!` at every function return

---

## x18 During Context Switch

When the scheduler switches tasks, `x18` must be saved for the outgoing task and
restored for the incoming task:

```asm
// arch/arm64/kernel/head.S cpu_switch_to:
cpu_switch_to:
    mov     x10, #THREAD_CPU_CONTEXT  // offset to cpu_context in task_struct
    add     x8, x0, x10              // x8 = &prev->thread.cpu_context
    mov     x9, sp                   // save current sp

    // Save callee-saved registers of OUTGOING task:
    stp     x19, x20, [x8], #16
    stp     x21, x22, [x8], #16
    stp     x23, x24, [x8], #16
    stp     x25, x26, [x8], #16
    stp     x27, x28, [x8], #16
    stp     x29, x9, [x8], #16      // fp + sp
    str     lr, [x8]                 // save LR

    // NOTE: x18 (SCS) is also saved:
#ifdef CONFIG_SHADOW_CALL_STACK
    str     x18, [x8]               // save SCS pointer
#endif

    add     x8, x1, x10             // x8 = &next->thread.cpu_context
    
    // Restore callee-saved registers of INCOMING task:
    ldp     x19, x20, [x8], #16
    ...
#ifdef CONFIG_SHADOW_CALL_STACK
    ldr     x18, [x8]               // restore SCS pointer
#endif

    msr     sp_el0, x1              // restore 'current'
    ...
    ret
```

Each task's `thread.cpu_context.x18` holds its shadow call stack pointer.
Context switches atomically update `x18`.

---

## `TSK_TI_SCS_SP` — The `thread_info` Field

```c
// arch/arm64/include/asm/thread_info.h:
struct thread_info {
    unsigned long flags;
    u64 ttbr0;
    union { ... preempt ... };
    u32 cpu;
    ...
#ifdef CONFIG_SHADOW_CALL_STACK
    void *scs_base;          // base of SCS buffer (for bounds checking)
    void *scs_sp;            // current SCS pointer ← TSK_TI_SCS_SP
#endif
};
```

`scs_sp` is the per-task SCS pointer. On context switch, this value is saved
and restored along with other callee-saved registers.

For `init_task`, `scs_sp` is initialized at compile time:
```c
// kernel/init_task.c:
struct task_struct init_task = {
    .thread_info = {
        .scs_base = init_shadow_call_stack,
        .scs_sp   = init_shadow_call_stack,  // starts at base
    },
    ...
};
```

---

## x18 During Exceptions and Interrupts

When an interrupt or exception occurs, the exception entry code saves and restores
all registers including `x18`. This is critical for SCS continuity:

```asm
// arch/arm64/kernel/entry.S kernel_entry macro:
...
stp     x28, x29, [sp, #16 * 14]  // save regular registers
...
// x18 is also saved:
stp     x18, xzr, [sp, #16 * 9]   // save x18 (SCS pointer) during interrupt
...
// At kernel_exit:
ldp     x18, xzr, [sp, #16 * 9]   // restore x18 (SCS pointer)
```

This ensures the SCS pointer is maintained even when the kernel is interrupted.
The interrupt handler has its OWN call stack (on the IRQ stack), but x18 is
preserved across the interrupt boundary.

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