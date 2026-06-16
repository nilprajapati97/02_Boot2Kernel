# Processor Modes at Each EL — SPSel and Stack Pointer Selection

**Category**: ARM64 Architecture Fundamentals  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

In AArch64, each Exception Level can use one of two stack pointers:
- **SP_EL0** — the "thread" stack pointer, shared across ELs conceptually (EL0's native SP).
- **SP_ELx** — the dedicated stack pointer for the current EL (SP_EL1 for EL1, SP_EL2 for EL2, SP_EL3 for EL3).

The selection of which stack pointer to use is controlled by `SPSel`, a 1-bit field in `PSTATE`. This determines whether the processor uses `SP_EL0` or `SP_ELx` for load/store operations using `SP` as base.

This design gives the processor two operating modes within each EL:
- **EL1t** (t = "thread") — uses `SP_EL0` as stack pointer. Used when handling exceptions on the user stack.
- **EL1h** (h = "handler") — uses `SP_EL1` as stack pointer. Used for normal kernel execution with kernel stack.

---

## 2. SPSel Register

`SPSel` is bit 0 of PSTATE and is accessed via the `MSR SPSel, #imm` instruction:

```asm
MSR SPSel, #0    // Use SP_EL0 as stack pointer (EL1t mode)
MSR SPSel, #1    // Use SP_ELx as stack pointer (EL1h mode)

MRS x0, SPSel    // Read current SPSel value
```

Or by directly manipulating PSTATE:
```asm
// The SPSR.M[0] bit encodes SPSel for the saved exception state
// When ERET restores SPSR_EL1 to PSTATE, SPSel is also restored
```

**Important**: At EL0, `SPSel=0` always. EL0 always uses `SP_EL0`. The `SPSel` selection is only meaningful at EL1 and above.

---

## 3. Stack Pointer Registers

ARM64 provides dedicated stack pointer registers per level:

```
SP_EL0  —  User-space stack pointer (EL0's stack)
           Also used by EL1 in EL1t mode (rare in Linux)
           
SP_EL1  —  EL1 (kernel) stack pointer
           Points to per-CPU kernel stack (typically 8KB or 16KB in Linux)
           
SP_EL2  —  EL2 (hypervisor) stack pointer
           Points to per-CPU hypervisor stack (KVM EL2 stack)
           
SP_EL3  —  EL3 (TF-A) stack pointer
           Points to per-CPU TF-A stack
```

**Banked nature**: `SP_EL1`, `SP_EL2`, `SP_EL3` are fully independent physical registers. They hold completely different values and are not automatically saved/restored by the CPU on exception entry/exit — that's done by kernel/hypervisor software.

`SP_EL0` is the only one shared: it always holds the EL0 stack pointer, and EL1 code can read/write it via `mrs x0, sp_el0` system register access.

---

## 4. Exception Vector Table Offsets and SP Mode

The ARM64 exception vector table is organized into 4 sections based on the source of exception and SP selection:

```
Vector table sections (each 0x200 bytes apart):
┌─ VBAR_EL1 + 0x000 ─┐  Exceptions from current EL, SP_EL0 (EL1t)
│  Synchronous         │
│  IRQ                 │
│  FIQ                 │
│  SError              │
├─ VBAR_EL1 + 0x200 ─┤  Exceptions from current EL, SP_EL1 (EL1h)
│  Synchronous         │  ← Linux kernel uses these vectors for
│  IRQ                 │     exceptions while in kernel mode
│  FIQ                 │
│  SError              │
├─ VBAR_EL1 + 0x400 ─┤  Exceptions from lower EL (EL0), AArch64
│  Synchronous         │  ← Linux uses these for SVC, data faults
│  IRQ                 │     from user space
│  FIQ                 │
│  SError              │
├─ VBAR_EL1 + 0x600 ─┤  Exceptions from lower EL (EL0), AArch32
│  Synchronous         │  ← For 32-bit compat apps
│  IRQ                 │
│  FIQ                 │
│  SError              │
└─────────────────────┘
```

When the kernel is executing with `SPSel=1` (EL1h mode) and a synchronous exception occurs, it is taken to `VBAR_EL1 + 0x200`.

---

## 5. Linux Kernel Stack Management

### 5.1 Kernel Stack Layout

Each task in Linux has a **kernel stack** allocated with `alloc_thread_stack_node()`:
- Size: `THREAD_SIZE` = 16 KB on ARM64 (two 4KB pages, or larger with `KASAN`).
- Lives in kernel virtual address space.
- `SP_EL1` points into this stack during kernel execution.

```c
// arch/arm64/include/asm/thread_info.h
struct thread_info {
    unsigned long       flags;          // low level flags
    u64                 ttbr0;          // saved user page table
    union {
        u64             preempt_count;
        struct {
            u32         count;
            u32         need_resched;
        };
    };
    u32                 cpu;            // CPU number
};

// Kernel stack layout (from low address to high address):
// [task_struct + thread_info] at bottom of stack
// [kernel stack grows down from top]
// [guard page at top and/or bottom for VMAP_STACK]
```

### 5.2 Entry to Kernel: SP_EL1 vs SP_EL0

When an exception is taken from EL0 (user space) to EL1 (kernel):
1. Hardware saves user `PSTATE` → `SPSR_EL1`, user `PC` → `ELR_EL1`.
2. Hardware **does NOT** automatically switch the stack pointer.
3. The kernel's exception handler must manually switch from `SP_EL0` (user stack) to `SP_EL1` (kernel stack).

```asm
// arch/arm64/kernel/entry.S — kernel_entry macro
// Called at EL1 after exception from EL0:
.macro kernel_entry, el, regsize = 64
    .if \el == 0
        // Coming from EL0: switch to kernel stack
        // SP currently = SP_EL0 (user stack) because
        // when EL=0 raised to EL1, SPSR.M[0]=0 means SP_EL0 was in use
        // and now we're at EL1h, so SP = SP_EL0 STILL (because SPSel=0
        // reflects what user was using)
        // Actually: on entry from EL0, SP = SP_EL0 still
        // We need to switch to the kernel stack:
        ldr     x25, [tsk, #TSK_STACK]     // Load kernel stack pointer
        add     sp, x25, #THREAD_SIZE      // SP = top of kernel stack
        // Save user registers to kernel stack:
        sub     sp, sp, #PT_REGS_SIZE
        stp     x0, x1, [sp, #16 * 0]
        // ... save all registers
    .else
        // Coming from EL1 (kernel itself): SP_EL1 is already kernel stack
        // Just allocate space on current kernel stack:
        sub     sp, sp, #PT_REGS_SIZE
        stp     x0, x1, [sp, #16 * 0]
        // ... save registers
    .endif
.endm
```

### 5.3 The kernel_exit macro and stack restoration

```asm
// arch/arm64/kernel/entry.S — kernel_exit macro
.macro kernel_exit, el
    .if \el == 0
        // Returning to EL0: restore user registers from kernel stack
        ldp     x0, x1, [sp, #16 * 0]
        // ... restore all registers
        // SP will revert to SP_EL0 after ERET because SPSR.M[0]=0
    .endif
    // Restore ELR_EL1, SPSR_EL1
    ldr     x21, [sp, #S_PC]
    ldr     x22, [sp, #S_PSTATE]
    msr     elr_el1, x21
    msr     spsr_el1, x22
    // Restore SP_EL0 (user stack pointer)
    ldr     x23, [sp, #S_SP]
    msr     sp_el0, x23
    // Restore all GPRs
    ldp     x0, x1, [sp, #16 * 0]
    // ... etc.
    eret
.endm
```

---

## 6. IRQ Stack on ARM64

Linux ARM64 has a dedicated **IRQ stack** separate from the task kernel stack, to prevent stack overflow on deeply nested interrupts:

```c
// arch/arm64/kernel/irq.c
// Per-CPU IRQ stack
DEFINE_PER_CPU(unsigned long [IRQ_STACK_SIZE/sizeof(long)], irq_stack);

// arch/arm64/include/asm/irq.h
#define IRQ_STACK_SIZE  THREAD_SIZE    // 16 KB

// When IRQ arrives during kernel execution:
// 1. Check if already on IRQ stack (prevent nesting)
// 2. Save current SP
// 3. Switch SP_EL1 to top of per-CPU IRQ stack
// 4. Handle IRQ
// 5. Restore original SP_EL1
```

This is why deep interrupt nesting does not overflow the task's kernel stack.

---

## 7. OVERFLOW_STACK — Stack Guard

ARM64 Linux with `CONFIG_VMAP_STACK` allocates the kernel stack in vmalloc space with a guard page:

```
[guard page (unmapped)]    ← Stack overflow page fault here
[kernel stack 16KB]
[guard page (unmapped)]    ← Stack underflow guard
```

If the kernel stack overflows, accessing the guard page triggers a data abort. The handler detects this and calls `panic("kernel stack overflow")`.

There's also a dedicated **overflow stack** per CPU (`overflow_stack`) used by the fault handler itself (since the normal stack is overflowed, you need somewhere to run the panic handler from).

---

## 8. PSCI and EL Transfer

ARM's Power State Coordination Interface (PSCI) uses `SMC` from EL1 to EL3 (TF-A) to control CPU power states:

```c
// arch/arm64/kernel/psci.c
int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
    // Issue SMC to TF-A at EL3
    // TF-A powers on the secondary CPU and drops it to EL1
    // Secondary CPU enters Linux at entry_point (secondary_entry)
}
```

When the secondary CPU starts in TF-A at EL3:
- `SP_EL3` is set up by TF-A.
- `SCTLR_EL3.M=0` initially.
- TF-A sets `SPSR_EL3.M[3:0]=0b0101` (EL1h mode), `SPSR_EL3.SP=1`.
- `ELR_EL3` = kernel's secondary entry point.
- `ERET` drops to EL1 with `SP_EL1` being used (EL1h mode).
- The secondary CPU enters Linux's secondary boot path.

---

## 9. Interview Questions & Answers

**Q1: What is the difference between EL1t and EL1h mode?**

In EL1t ("thread mode"), `PSTATE.SP=0`, so the processor uses `SP_EL0` as the stack pointer. In EL1h ("handler mode"), `PSTATE.SP=1`, so `SP_EL1` is used. Linux always runs the kernel in EL1h mode (`SPSel=1`), using the dedicated kernel stack (`SP_EL1`). The exception vector entries at `VBAR_EL1 + 0x200` are for EL1h, meaning kernel-to-kernel exceptions (IRQ during kernel execution, kernel bug faults) use these vectors.

**Q2: When an IRQ fires while executing user-space code, what stack does the IRQ handler use?**

The IRQ is taken to EL1h. The exception entry code (`kernel_entry 0`) switches `SP` from the current value (still pointing to `SP_EL0` conceptually after EL0→EL1 transition) to the task's kernel stack. If `CONFIG_IRQ_STACKS` is set, the handler then further switches to the per-CPU IRQ stack before invoking the actual interrupt handler.

**Q3: How does Linux save SP_EL0 (user stack pointer) across a system call?**

In `kernel_entry 0` (exception from EL0), the macro saves `SP_EL0` to `struct pt_regs.sp` on the kernel stack:
```asm
mrs x21, sp_el0        // Read user stack pointer
str x21, [sp, #S_SP]   // Save to pt_regs
```
On return (`kernel_exit 0`), it restores it:
```asm
ldr x21, [sp, #S_SP]   // Load from pt_regs
msr sp_el0, x21        // Restore user stack pointer
```
This ensures the user's stack pointer is preserved across the system call.

**Q4: Can kernel code read or write SP_EL0 directly?**

Yes. `SP_EL0` is accessible from EL1 as a system register:
```asm
mrs x0, sp_el0     // Read user stack pointer from EL1
msr sp_el0, x1     // Write user stack pointer from EL1
```
Linux does this in `copy_thread()` to set up the initial user stack pointer for a new process, and in context switch code to restore the user stack pointer.

**Q5: What happens to SP_EL1 during a context switch?**

During `cpu_switch_to(prev, next)` in `arch/arm64/kernel/entry.S`:
1. The current kernel stack pointer (`SP_EL1`) is saved to `prev->thread.cpu_context.sp`.
2. The next task's `SP_EL1` is loaded from `next->thread.cpu_context.sp`.
3. The kernel continues executing on the new task's kernel stack.
The switch is atomic from the processor's perspective — once `SP_EL1` is restored, we're "on" the next task's stack.

---

## 10. Quick Reference

| Symbol | Register | Used At | Content |
|---|---|---|---|
| SP (when SPSel=0) | SP_EL0 | EL0, EL1t | User/thread stack |
| SP (when SPSel=1) | SP_EL1 | EL1h | Kernel task stack |
| SP_EL2 | SP_EL2 | EL2 | Hypervisor stack |
| SP_EL3 | SP_EL3 | EL3 | TF-A stack |

| PSTATE.M encoding | Mode | SP used |
|---|---|---|
| `0b0000` | EL0t | SP_EL0 |
| `0b0100` | EL1t | SP_EL0 |
| `0b0101` | EL1h | SP_EL1 |
| `0b1000` | EL2t | SP_EL0 |
| `0b1001` | EL2h | SP_EL2 |
| `0b1100` | EL3t | SP_EL0 |
| `0b1101` | EL3h | SP_EL3 |
