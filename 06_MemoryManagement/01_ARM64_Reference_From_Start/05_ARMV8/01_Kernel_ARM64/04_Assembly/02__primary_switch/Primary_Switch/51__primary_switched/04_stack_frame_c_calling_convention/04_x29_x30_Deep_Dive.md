# x29 and x30 Registers — Architecture Deep Dive

## x29 — The Frame Pointer Register

ARM64 designates `x29` (also called `fp`) as the frame pointer by convention
(AAPCS64). The hardware does NOT enforce this — `x29` is a general-purpose register
at the hardware level. The convention is enforced by:
1. The ABI (callers/callees agree to save/restore it correctly)
2. The compiler (`-fno-omit-frame-pointer` ensures `x29` always holds the current frame base)
3. Tools (GDB, perf, libunwind all expect `x29` = frame pointer)

**`x29` encoding:** In A64 instruction encoding, register 29 (0b11101) represents `fp`.
In load/store pairs: `stp x29, x30, [sp, #-16]!` encodes as:
```
bits [31:30] = 10  (64-bit)
bits [29:27] = 101 (STP with pre-index)
bits [21:15] = 1111110  (imm7 = -2 after shift, meaning -16 bytes)
bits [14:10] = 11110   (Rt2 = x30)
bits [9:5]   = 11111   (Rn = sp = x31 in STP context)
bits [4:0]   = 11101   (Rt = x29)
```

---

## x30 — The Link Register

`x30` (also called `lr`) is architecturally defined as the link register. When the
CPU executes `bl <target>`:
1. Hardware writes `PC + 4` (return address) into `x30`
2. Hardware jumps to `<target>`

`bl` is "Branch with Link" — the link IS `x30`. This is ARM's version of x86's
`call` instruction (which pushes the return address to the stack instead).

Key difference from x86:
- x86 `call`: pushes return address to stack, no register used
- ARM64 `bl`: puts return address in x30, does NOT touch the stack

This means ARM64 LEAF functions (no further `bl`) never need to save x30:
```asm
// Leaf function — no stp needed:
my_leaf:
    add     x0, x0, x1
    ret                    // x30 still has return address from caller's bl
```

Non-leaf functions MUST save x30 because the next `bl` would overwrite it:
```asm
// Non-leaf function — must save x30:
my_nonleaf:
    stp     x29, x30, [sp, #-16]!  // save x30 before it gets overwritten
    mov     x29, sp
    bl      some_other_function     // overwrites x30 with new return address
    ldp     x29, x30, [sp], #16   // restore original x30
    ret                            // return to our caller
```

---

## `__primary_switched` Context — Why Save x30?

In `__primary_switched`:
```asm
stp     x29, x30, [sp, #-16]!   // save before bl start_kernel overwrites x30
mov     x29, sp
...
bl      start_kernel             // x30 = __primary_switched return address (overwritten)
```

`bl start_kernel` will overwrite `x30` with `__primary_switched_after_start_kernel + 4`
(the instruction after the `bl`). If we hadn't saved the original `x30`, it would
be lost. Not that it matters — boot never returns — but the ABI requires it for
correct frame-chain construction.

---

## The `ret` Instruction — How Return Works

`ret` is shorthand for `ret x30`, which means "branch to address in x30":
```asm
// ret decodes to:
br      x30    // unconditional branch to address in register x30
```

In `start_kernel`, the prologue saves `x30`:
```asm
start_kernel:
    stp     x29, x30, [sp, #-272]!  // x30 = return address (inside __primary_switched)
    mov     x29, sp
    ...
    ldp     x29, x30, [sp], #272    // restore x29, x30
    ret                              // branch to x30 = __primary_switched return point
```

This is why `__primary_switched` must save `x30` before `bl start_kernel` —
so that when `start_kernel` eventually tries to return (it won't, but the code
must be valid), it would return correctly to `__primary_switched`.

---

## `x30` in Panic/BUG Traces

When the kernel crashes, the panic output includes:
```
PC is at start_kernel+0x1c0
LR is at __primary_switched+0x5c
```

"LR" = Link Register = x30. This shows that `start_kernel` was called from
`__primary_switched+0x5c` (the instruction after `bl start_kernel`). The kernel's
`show_regs()` function reads x30 to print the LR:

```c
// arch/arm64/kernel/process.c
void show_regs(struct pt_regs *regs)
{
    ...
    pr_info("x30: %016llx\n", regs->regs[30]);  // LR = x30
    ...
```

This is only possible because the frame was set up correctly with `stp x29, x30`.

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