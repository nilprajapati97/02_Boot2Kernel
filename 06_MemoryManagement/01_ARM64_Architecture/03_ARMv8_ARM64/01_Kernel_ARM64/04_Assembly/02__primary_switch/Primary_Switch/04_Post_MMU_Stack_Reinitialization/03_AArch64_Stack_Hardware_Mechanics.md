# AArch64 Stack Hardware Mechanics

**Context:** How the ARM64 hardware enforces and uses the stack pointer  
**Architecture:** AArch64 (ARMv8-A), EL1 mode (kernel)

---

## 0. ARM64 Has TWO Stack Pointers

Unlike x86 (one SP per privilege level), ARM64 has two EL1 stack pointers:

```
SP_EL0  — EL0 stack pointer (also accessible at EL1 when PSTATE.SPSel = 0)
SP_EL1  — EL1 stack pointer (used when PSTATE.SPSel = 1)
```

The `PSTATE.SPSel` bit selects which SP is active:
- `SPSel = 0`: `sp` refers to `SP_EL0` (even when executing at EL1)
- `SPSel = 1`: `sp` refers to `SP_EL1` (normal kernel operation)

**Why does this exist?**

The `EL1h` exception mode (versus `EL1t`):
- `EL1h` = EL1 with `SPSel = 1` → uses `SP_EL1`
- `EL1t` = EL1 with `SPSel = 0` → uses `SP_EL0`

Exception handlers can use a known-clean `SP_EL1` even while the user mode
`SP_EL0` holds a potentially corrupt value (from a user crash). This prevents
a corrupt user SP from breaking the kernel's exception handler.

Linux always runs at EL1 with `SPSel = 1` (using `SP_EL1`).

---

## 1. Stack Alignment Requirement

AArch64 ABI requires the stack to be **16-byte aligned** at any function call:

```
sp must satisfy: sp % 16 == 0

At any BL instruction: sp must be 16-byte aligned
At any exception entry: hardware pushes frame to [sp - 16] (must also be aligned)
```

**Hardware enforcement:** If the CPU accesses the stack at a non-16-byte aligned
address AND `SCTLR_EL1.SA` (Stack Alignment check) = 1:

```
Stack Alignment Fault triggered
ESR_EL1.EC = 0x26 (SP alignment fault)
```

`__cpu_setup` sets `SCTLR_EL1.SA = 1` (enabled in `INIT_SCTLR_EL1_MMU_ON`).
Any function in the kernel that incorrectly aligns the stack will trigger this.

**`early_init_stack` alignment:** The linker places it after `ALIGN(PAGE_SIZE)`,
which ensures the backing memory starts on a 4KB boundary. The `early_init_stack`
symbol itself is at the top of the 4KB area — also page-aligned → satisfies
16-byte alignment requirement.

**`init_thread_union` alignment:** Placed with `__aligned(THREAD_ALIGN)` where
`THREAD_ALIGN = THREAD_SIZE = 16384` — strongly aligned.

---

## 2. Stack Pointer Writeback Instructions

ARM64 has two variants of stack-modifying instructions:

### Pre-indexed (PUSH equivalent)

```asm
stp     x29, x30, [sp, #-16]!    // sp -= 16; mem[sp] = x29; mem[sp+8] = x30
```

The `!` indicates **writeback**: `sp` is decremented first, then the store
executes. This is the AArch64 equivalent of x86's `push`.

### Post-indexed (POP equivalent)

```asm
ldp     x29, x30, [sp], #16      // x29 = mem[sp]; x30 = mem[sp+8]; sp += 16
```

`sp` is incremented **after** the load. Equivalent to x86's `pop`.

---

## 3. Stack Frame Layout (AAPCS64)

The AAPCS64 defines the standard stack frame layout:

```
High address (frame base from caller's perspective)
    ┌───────────────────────────┐
    │  Caller's Frame Pointer   │  [sp+8]  (x29, previous frame's FP)
    │  Return Address           │  [sp+0]  (x30, LR from caller)
    └───────────────────────────┘ ← Frame pointer (x29) points here
    ┌───────────────────────────┐
    │  Callee-saved registers   │  (x19-x28 as needed)
    ├───────────────────────────┤
    │  Local variables          │  (compiler-determined)
    ├───────────────────────────┤
    │  (padding to 16B align)   │  (if needed)
    └───────────────────────────┘ ← Stack pointer (sp) after prologue
Low address (stack grows down)
```

### Prologue of a Typical C Function

```asm
// void func(void):
stp     x29, x30, [sp, #-96]!   // Allocate 96 bytes, save FP+LR
mov     x29, sp                  // Set new frame pointer
stp     x19, x20, [sp, #16]     // Save callee-saved x19, x20
stp     x21, x22, [sp, #32]     // Save x21, x22
...                              // Local variable setup
```

### Epilogue

```asm
ldp     x19, x20, [sp, #16]     // Restore x19, x20
ldp     x21, x22, [sp, #32]     // Restore x21, x22
ldp     x29, x30, [sp], #96     // Restore FP+LR, deallocate frame
ret                               // Branch to x30 (return address)
```

---

## 4. Stack Overflow Detection Hardware Mechanism

### Traditional: No Hardware Detection

Without Pointer Authentication (PAC) or Memory Tagging Extension (MTE):
- Stack overflows silently corrupt adjacent memory
- The kernel uses **stack canaries** (a known value at stack base; checked in epilogue)
- Canary check at function return (if `-fstack-protector-strong` or similar)

### ARM64 Shadow Call Stack (SCS)

ARMv8.3+ with the kernel's `CONFIG_SHADOW_CALL_STACK`:

```
Regular stack: holds local variables, callee-saved registers
Shadow stack:  holds ONLY return addresses

x18 = shadow stack pointer (gp register, dedicated to SCS in Linux)
```

Function prologue with SCS:
```asm
str     x30, [x18], #8          // Store LR to shadow stack, advance x18
stp     x29, x30, [sp, #-16]!  // Normal frame setup
```

Epilogue:
```asm
ldr     x30, [x18, #-8]!       // Load LR from shadow stack
ldp     x29, xzr, [sp], #16   // Restore FP (x30 already loaded from shadow stack)
ret
```

If the regular stack is corrupted (overflow), the return address still comes
from the shadow stack → the corruption cannot redirect execution (ROP mitigation).

### Pointer Authentication (PAC)

With `CONFIG_ARM64_PTR_AUTH`:

```asm
// Prologue:
paciasp                          // Sign LR using SP as context → PAC in high bits
stp     x29, x30, [sp, #-16]!
mov     x29, sp

// Epilogue:
ldp     x29, x30, [sp], #16
autiasp                          // Authenticate LR: fail if PAC doesn't match
ret
```

An attacker who overwrites the saved LR would produce a bad PAC → `autiasp`
triggers a PAC fault → crash rather than code execution.

---

## 5. The `init_task` Stack and `sp_el0`

In `__primary_switched`:

```asm
adrp    x4, init_thread_union
add     sp, x4, #THREAD_SIZE    // SP = top of init_task's kernel stack

adrp    x5, init_task
msr     sp_el0, x5              // SP_EL0 = pointer to init_task
```

`SP_EL0` is used at EL1 to hold the **current task pointer**. This is the
`current` macro's hardware backing:

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

So `current` in kernel code reads the `SP_EL0` register. After `__primary_switched`,
`current = &init_task` (the idle thread / PID 0). This must be set before any
code path that calls `current`.

---

## 6. Stack Virtualization — Each Task Gets Its Own Stack

After the scheduler is initialized, each kernel thread and each user process
has its own kernel stack (stored in `task_struct.stack`):

```
task_struct:
    void *stack = &thread_info (8 bytes at task_struct.stack[0])

Stack layout per task:
    [THREAD_SIZE - 1] = top
    [0]               = thread_info (struct at bottom of stack)
```

On context switch (`__switch_to`):
```asm
// arch/arm64/kernel/entry.S or process.c
msr     sp_el0, x1              // x1 = next->task_struct address
...
// sp is updated by the context switch to next task's kernel stack top
```

---

## 7. Hardware Exception and Stack Interaction

When an exception occurs (e.g., system call, IRQ, data abort):

```
Hardware actions (automatic):
1. PSTATE saved to SPSR_EL1 (including SPSel bit)
2. PC saved to ELR_EL1
3. PSTATE.SPSel set to 1 (use SP_EL1)
4. PSTATE.DAIF masks set (disable interrupts)
5. PC jumps to VBAR_EL1 + vector offset

Software then:
1. Exception entry code (vectors table) runs
2. Saves all registers to kernel stack (SP_EL1)
3. Calls C handler
4. Restores registers from stack
5. ERET: restores PC from ELR_EL1, PSTATE from SPSR_EL1
```

The hardware-enforced separation of `SP_EL0` (user stack) and `SP_EL1` (kernel
stack) ensures exceptions always land on the kernel stack, regardless of where
the user's SP was pointing.

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