# `PT_REGS_SIZE` Reservation — Why the Stack Starts Below the Top

## The Counter-Intuitive Stack Top

After `init_cpu_task` completes, `sp` does NOT point to the very top of `init_stack`.
Instead:
```
sp = &init_stack[0] + THREAD_SIZE - PT_REGS_SIZE
   = init_stack_base + 16384 - 336
   = init_stack_base + 16048
```

There are 336 bytes ABOVE `sp` — reserved for `struct pt_regs`.

---

## Why Reserve `pt_regs` at the Stack Top?

### Reason 1: The Stack Overflow Detection Mechanism

ARM64 Linux uses the stack pointer arithmetic:
```c
#define task_stack_page(task)  ((void *)(task)->stack)
#define task_pt_regs(task)     \
    ((struct pt_regs *)(task_stack_page(task) + THREAD_SIZE) - 1)
```

This ALWAYS expects `pt_regs` to be at the TOP of the stack. Every kernel task,
including the boot task, must have this layout. The boot task (`init_task`) must
conform.

### Reason 2: Exception Entry Model

On ARM64, when the CPU takes an exception (syscall, IRQ, fault) from user space:
```asm
// kernel/entry.S kernel_entry macro:
sub     sp, sp, #PT_REGS_SIZE       // reserve pt_regs on stack
stp     x0, x1, [sp, #16 * 0]      // save x0, x1
stp     x2, x3, [sp, #16 * 1]      // save x2, x3
...
mrs     x21, sp_el0                 // save user SP
stp     x30, x21, [sp, #S_LR]      // save LR + user SP
```

For tasks OTHER than `init_task`, `sp` at exception entry = `sp = stack_top - PT_REGS_SIZE`,
same as where `init_cpu_task` positions `sp` for `init_task`.

`init_task` just happens to ALREADY have `sp` set to this position (done at boot).
Other tasks get their `sp` set here on every kernel entry from user space.

### Reason 3: The `task_pt_regs(current)` Fast Path

```c
// In syscall entry handlers:
struct pt_regs *regs = task_pt_regs(current);
// This is:
// (struct pt_regs *)(init_task.stack + THREAD_SIZE) - 1
// = (struct pt_regs *)(init_stack + 16384) - 1
// = (struct pt_regs *)(init_stack + 16384 - 336)
// = the SAME address as 'sp' after init_cpu_task!
```

The `task_pt_regs(current)` macro gives the same address as `sp` after
`init_cpu_task`. This is NOT a coincidence — it's designed this way so that
`pt_regs` is always at a predictable location.

---

## What Happens to the 336-byte pt_regs During Normal Boot

During normal boot (no syscalls, no exceptions from user space to `init_task`):

- The 336 bytes at `[sp+0 .. sp+335]` are mostly unused
- `[sp + S_STACKFRAME]` = `pt_regs.stackframe` is explicitly initialized:
  - `[sp + S_STACKFRAME + 0]` (fp) = 0 ← unwind sentinel
  - `[sp + S_STACKFRAME + 8]` (pc) = 0
  - `[sp + S_STACKFRAME_TYPE]` = 1 (FRAME_META_TYPE_FINAL)
- All other bytes in the reservation are zeros (init_stack is zero-initialized)

When `bl start_kernel` is called, the CPU pushes a new frame BELOW `sp`:
```asm
bl start_kernel:
    // ARM64 bl does NOT push anything automatically
    // The CALLEE does: stp x29, x30, [sp, #-framesize]!
```

The caller (`__primary_switched`) sets up x29 BEFORE the bl. But the boot code
doesn't have a traditional AAPCS64 frame at `__primary_switched` — the frame
sentinel at `[sp + S_STACKFRAME]` IS the "boot frame."

---

## Stack Usage Watermark After Full Boot

After `start_kernel` runs (and the kernel has booted completely):
```
init_stack usage (approximate):
    [16384..16048]: pt_regs reservation (336 bytes, frame sentinel here)
    [16048..15000]: start_kernel + setup_arch + ... frames (~1KB)
    [15000..14000]: more init functions
    ...
    [max usage ~4-5KB]: deep nesting during hardware init
    [unused ~10KB]: never touched
```

The kernel uses at most 4-6KB of the 16KB stack during early init, leaving ~10KB
of headroom. This headroom is important for unexpected deep recursion in device
drivers during `initcall` processing.

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