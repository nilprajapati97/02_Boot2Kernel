# Stack Growth Direction and ARM64 Stack Mechanics

## Stack Grows DOWN on ARM64

ARM64 uses a **descending full** stack:
- The stack grows toward LOWER addresses
- `sp` always points to the LAST WRITTEN value (the "full" element)
- A `push` operation DECREMENTS `sp`, then writes
- A `pop` operation READS from `sp`, then INCREMENTS `sp`

```
Memory layout (higher addresses at top):
┌─────────────────────┐  ← stack_base + THREAD_SIZE (highest VA)
│   pt_regs (336B)    │  ← reserved by init_cpu_task
│                     │
├─────────────────────┤  ← sp (after init_cpu_task)
│  [available space]  │  ← stack grows DOWN from sp
│  [frame for A]      │  A calls B: sp -= frame_size
│  [frame for B]      │  B calls C: sp -= frame_size
│  [frame for C]      │  ← sp moves further down
│                     │
└─────────────────────┘  ← stack_base (lowest VA, guarded by VMAP_STACK)
```

---

## AAPCS64 Stack Operations

The ARM64 Procedure Call Standard (AAPCS64) defines the stack ABI:

### Function Prologue (callee)
```asm
// Standard AAPCS64 prologue:
stp     x29, x30, [sp, #-frame_size]!  // pre-decrement sp, save fp+lr
mov     x29, sp                          // update frame pointer

// If more space needed:
sub     sp, sp, #local_var_size
```

The `!` (writeback) in `stp x29, x30, [sp, #-N]!`:
1. Computes address: `sp - N`
2. Stores `x29` at `sp - N`
3. Stores `x30` at `sp - N + 8`
4. Updates `sp = sp - N`

This is a 3-in-1 operation.

### Function Epilogue (callee)
```asm
// Standard epilogue:
ldp     x29, x30, [sp], #frame_size   // post-increment sp, restore fp+lr
ret                                    // jump to lr
```

---

## SP Alignment Requirement

ARM64 ABI requires `sp` to be **16-byte aligned** at ALL times during a function call.
The `sp` value when making a function call via `bl` must be 16-byte aligned.

`init_cpu_task` computes:
```
sp = init_stack_base + THREAD_SIZE - PT_REGS_SIZE
   = init_stack_base + 16384 - 336
   = init_stack_base + 16048
```

Is 16048 divisible by 16? 16048 / 16 = 1003 → YES, exactly 16-byte aligned.

Both `THREAD_SIZE` (16384) and `PT_REGS_SIZE` (336) are multiples of 16:
- 16384 = 1024 × 16
- 336 = 21 × 16

Therefore their difference is also a multiple of 16. Combined with `init_stack`
being `THREAD_SIZE`-aligned (16384-byte boundary = power of 2), `sp` is guaranteed
to be 16-byte aligned.

---

## SP Register on ARM64 — Special Properties

ARM64's `sp` register has special architectural behavior:
- It's NOT `x31` (which is `xzr`/`wzr`)
- It CAN be used in arithmetic: `add sp, sp, #N` is valid
- It CANNOT be used as a source operand in most data processing instructions
- Any access to `sp` as an address MUST be 16-byte aligned (hardware enforces this!)

If `sp` is not 16-byte aligned when used as a memory address:
```
Data Abort: Alignment fault (EC=0x25) → crashes the kernel
```

This hardware enforcement is why Linux is careful to maintain 16-byte `sp` alignment.
The `PT_REGS_SIZE = 336` was designed to be a multiple of 16 for this reason.

---

## Stack Overflow Detection on ARM64

Without `CONFIG_VMAP_STACK` (no guard pages):
```
// DANGEROUS: no detection
if (sp < stack_base) {
    // Stack overflow — writing over adjacent kernel data
    // No detection until corruption causes an unrelated crash
}
```

With `CONFIG_VMAP_STACK`:
```
// vmalloc-based stacks with guard pages:
// Virtual layout:
[guard page - unmapped]  ← overflow here → Translation Fault
[16KB stack]
[pt_regs reservation]

// When overflow occurs:
// Translation Fault at EL1 → ESR_ELx shows SP below stack
// Kernel catches it in the fault handler → prints stack overflow warning
// Uses the emergency stack (irq_stack_ptr) to handle the fault
```

`init_stack` itself uses neither (it's statically allocated). Overflow of
`init_stack` during early boot would silently corrupt kernel data.

---

## Interview Angle: Stack Depth During Early Init

**Q: How deep is the call stack when `start_kernel` is entered?**

```
__primary_switched (head.S)     ← x29 = boot frame sentinel (frame 0)
    └── bl start_kernel          ← frame 1 (pushed by start_kernel's prologue)
        └── setup_arch           ← frame 2
            └── ...              ← deeper frames
```

The `start_kernel` function itself does NOT have a traditional AAPCS64 prologue
because it's called from assembly. The frame record at `[sp + S_STACKFRAME]`
set by `init_cpu_task` IS the "frame 0" — the boot frame.

The unwinder chain:
```
current frame (deepest) → ... → start_kernel → __primary_switched boot frame (fp=0) → STOP
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