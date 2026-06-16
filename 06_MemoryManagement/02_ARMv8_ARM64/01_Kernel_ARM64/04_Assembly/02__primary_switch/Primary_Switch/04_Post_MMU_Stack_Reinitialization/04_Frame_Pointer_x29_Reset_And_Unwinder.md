# Frame Pointer x29 Reset and the Stack Unwinder

**Context:** Why `__primary_switched` sets `mov x29, xzr` (x29 = 0)  
**Source:** `arch/arm64/kernel/head.S`, `arch/arm64/kernel/stacktrace.c`

---

## 0. What Is the Frame Pointer?

In AAPCS64, register `x29` is the **Frame Pointer** (FP). It serves as an anchor
for the current stack frame:

```
Function call chain: A → B → C

Stack layout (high to low):
┌──────────────────────────┐
│ A's LR (return to A's caller) │
│ A's x29 (A's caller's FP)     │ ← x29 when in A
├──────────────────────────┤
│ B's LR (return to A)          │
│ B's x29 (A's FP value)        │ ← x29 when in B
├──────────────────────────┤
│ C's LR (return to B)          │
│ C's x29 (B's FP value)        │ ← x29 when in C (current)
└──────────────────────────┘
         ↑
         sp
```

Each `{x29, x30}` pair forms a **linked list of stack frames**, where:
- `x29` (frame pointer) = address of current frame's `{x29, x30}` pair
- `x30` (link register) = return address to caller

The unwinder follows this chain: current `x29` → load `{prev_fp, ret_addr}` →
`x29 = prev_fp` → repeat until `x29 = 0` (chain terminator).

---

## 1. AArch64 Frame Pointer Convention

The standard prologue that sets up the frame pointer chain:

```asm
// function prologue:
stp     x29, x30, [sp, #-N]!   // Save FP and LR, allocate frame
mov     x29, sp                  // x29 = current sp (= address of saved FP+LR)

// function epilogue:
ldp     x29, x30, [sp], #N     // Restore FP and LR, deallocate frame
ret                               // x30 = return address
```

After `mov x29, sp`, the frame pointer register `x29` points to the exact
location in the stack where the previous frame pointer is stored.

---

## 2. Why `__primary_switched` Sets `x29 = 0`

```asm
// arch/arm64/kernel/head.S — __primary_switched:
SYM_FUNC_START_LOCAL(__primary_switched)
    ...
    mov     x29, xzr               // Clear frame pointer — bottom of call stack
    ...
    bl      start_kernel           // C function — creates its own frame
```

Before this reset, `x29` holds whatever value it had from the pre-MMU world:
- A frame pointer pointing into the old `early_init_stack`
- That stack is now at a **physical address** that is either:
  - Not mapped in TTBR1 (kernel VA range)
  - Potentially mapped in TTBR0 (identity map) but PAN-protected

If the unwinder tried to follow the old `x29` chain:

```
Start: x29 = 0x4000_2000 (old PA-as-VA from early_init_stack)
Walk frame: load {prev_fp, ret_addr} from VA 0x4000_2000

→ With PAN enabled: Access to user-range VA from EL1 → PAN fault → kernel crash
→ Without PAN: May succeed but reads garbage from pre-MMU stack data
```

Setting `x29 = 0` (NULL) terminates the frame pointer chain cleanly.

---

## 3. The ARM64 Stack Unwinder

```c
// arch/arm64/kernel/stacktrace.c (simplified)

struct frame_info {
    unsigned long fp;   // current frame pointer (x29 value)
    unsigned long pc;   // current program counter
};

int unwind_frame(struct task_struct *tsk, struct frame_info *frame)
{
    unsigned long fp = frame->fp;
    
    // Termination condition: fp == 0
    if (fp == 0)
        return -ENODATA;  // End of call stack
    
    // Alignment check
    if (fp & 0xf)
        return -EINVAL;   // Misaligned FP — corrupt stack
    
    // Range check: fp must be within task's stack
    if (!on_accessible_stack(tsk, fp, 16, NULL))
        return -EINVAL;   // FP points outside valid stack range
    
    // Load next frame: {prev_fp, return_pc}
    frame->fp = READ_ONCE_NOCHECK(*(unsigned long *)(fp));
    frame->pc = READ_ONCE_NOCHECK(*(unsigned long *)(fp + 8));
    
    return 0;
}
```

The key termination check: `if (fp == 0) return -ENODATA;`

Setting `x29 = 0` in `__primary_switched` ensures the very first call stack
walk from `start_kernel` terminates cleanly. Without this:
- The unwinder would try to follow the pre-MMU `x29` value
- Either it faults (PAN or unmapped memory) or prints garbage
- Kernel panics during early boot (`start_kernel`) would produce wrong backtraces

---

## 4. Frame Pointer Enforcement: `-fno-omit-frame-pointer`

The Linux kernel is built with:

```
arch/arm64/Makefile:
    KBUILD_CFLAGS += -fno-omit-frame-pointer
```

Without this flag, GCC/Clang may use `x29` as a general-purpose register
(omitting the frame pointer setup), which would:
- Slightly improve performance (one fewer register load/store per function)
- Break all stack unwinding (backtraces would be empty or wrong)

The kernel requires correct backtraces for:
- Kernel oops/panic printouts
- `perf` profiling (call graph collection)
- `kgdb` debugging
- `ftrace` function graph tracing

So `-fno-omit-frame-pointer` is mandatory.

---

## 5. ORC Unwinder vs. Frame Pointer Unwinder

Linux ARM64 uses **Frame Pointer** unwinding (not the ORC unwinder used on x86):

```
x86 kernel:   ORC (Oops Rewind Capability) — uses DWARF-like metadata tables
ARM64 kernel: Frame Pointer — walks x29 chain
```

Advantages of Frame Pointer on ARM64:
- Simple, reliable, no external tables needed
- Works even after stack corruption (if FP chain itself is intact)
- Minimal overhead (~2 instructions per function)

Disadvantages:
- x29 is unavailable for general-purpose use (wastes one register)
- Leaf functions with no calls still need FP setup (overhead)

---

## 6. Shadow Call Stack and Frame Pointer Interaction

With `CONFIG_SHADOW_CALL_STACK`:

```asm
// Prologue with SCS:
str     x30, [x18], #8          // Save LR to shadow stack
stp     x29, x30, [sp, #-16]!  // ALSO save LR to regular stack (for unwinder)
mov     x29, sp
```

The frame pointer chain is maintained on the **regular stack**, not the shadow
stack. The shadow stack holds LR for security (return address protection), but
the unwinder still uses the x29 chain on the regular stack.

The `x29 = 0` reset in `__primary_switched` works the same way regardless of
SCS being enabled.

---

## 7. Frame Pointer in Context: The Full `__primary_switched` Setup

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    // 1. Stack reinit (from previous document)
    adrp    x4, init_thread_union
    add     sp, x4, #THREAD_SIZE

    // 2. Store task pointer in SP_EL0
    adrp    x5, init_task
    msr     sp_el0, x5

    // 3. Frame pointer reset ← THIS DOCUMENT
    mov     x29, xzr               // Terminate frame pointer chain

    // 4. Link register clear (optional but clean)
    mov     x30, xzr               // x30 = 0 — no "return to caller"

    // 5. Call start_kernel
    bl      start_kernel
    // (never returns)
```

After `bl start_kernel`, the compiler generates:
```asm
// start_kernel prologue:
stp     x29, x30, [sp, #-96]!  // Saves x29=0 (FP) and x30=0 (or addr of panic)
mov     x29, sp
```

The saved `x29 = 0` in `start_kernel`'s frame becomes the terminator:

```
Backtrace walk from inside start_kernel:
  1. load {prev_fp, ret_pc} from current x29
  2. prev_fp = 0 → STOP
  
Output:
  start_kernel (arch/arm64/kernel/head.S:XXX)
  (no further frames — terminated cleanly)
```

---

## 8. What a Good Backtrace Looks Like vs. Bad

**Good (x29 properly reset to 0):**
```
Call trace:
 dump_stack+0x88/0xbc
 early_printk+0x1f4/0x240
 setup_arch+0xc4/0x380
 start_kernel+0x7c/0x340
```

**Bad (x29 not reset — stale value from pre-MMU stack):**
```
Call trace:
 dump_stack+0x88/0xbc
 early_printk+0x1f4/0x240
 setup_arch+0xc4/0x380
 start_kernel+0x7c/0x340
 (corrupted frame pointer — 0x40008000 not in valid stack range)
 PANIC: corrupted stack end detected
```

Or with PAN enabled:
```
PANIC: Unable to handle kernel paging request at virtual address 0000000040008000
(PAN violation at EL1 — access to user VA from kernel)
```

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