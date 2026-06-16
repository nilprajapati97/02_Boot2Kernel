# Shadow Call Stack — Security Architecture Overview

## The Attack Model: Return-Oriented Programming (ROP)

ROP attacks exploit stack buffer overflows to corrupt return addresses:

```
NORMAL execution:
    foo() calls bar():
        ret_addr_to_foo is on the stack at [sp+8]
        bar() returns: loads [sp+8] → PC = foo()

ROP ATTACK:
    Attacker writes:
        [sp+8] = gadget1_address    (hijacked return address)
        gadget1: pop x0; ret        (loads attacker's x0, returns to...)
        ...                         (chain of gadgets → arbitrary execution)
```

The attacker can chain arbitrary "gadgets" (small code sequences ending in `ret`)
to execute arbitrary operations — even without injecting new code (bypasses NX/XN).

---

## The SCS Solution: A Second Hidden Stack

Shadow Call Stack maintains a SECOND call stack in a separate memory region.
Return addresses are stored there (in addition to the normal stack). On return,
the hardware verifies the two copies match.

```
NORMAL stack (kernel):
    [sp+8] = return_addr           ← attacker can overwrite this

SHADOW stack (x18 register):
    [x18-8] = return_addr          ← in a separate, guarded memory region
```

At function entry (compiler-generated):
```asm
// SCS prologue (added by compiler -fsanitize=shadow-call-stack):
str    x30, [x18], #8    // push LR onto shadow stack (x18 advances by 8)
stp    x29, x30, [sp, #-frame]!  // normal stack frame (also saves LR)
mov    x29, sp
```

At function return (compiler-generated):
```asm
ldp    x29, x30, [sp], #frame    // restore from normal stack
ldr    x30, [x18, #-8]!          // pop from shadow stack (verify/restore LR)
ret                               // use shadow stack's LR (trusted!)
```

Even if the attacker corrupted `[sp+8]` (the normal stack return address), the
`ldr x30, [x18, #-8]!` instruction OVERWRITES x30 with the shadow stack's copy.
The ROP chain is neutralized.

---

## `x18` — The Shadow Stack Pointer Register

`x18` is a general-purpose register in AAPCS64 (the standard ABI). Normally,
it's caller-saved — functions are free to use it.

Linux REDEFINES `x18` as a reserved register:
- The kernel is compiled with `-ffixed-x18` (GCC/Clang flag)
- No generated code uses `x18` for general purposes
- `x18` is exclusively the shadow call stack pointer

This is enforced at compile time — any kernel code that accidentally uses `x18`
causes a compile error.

---

## `scs_load_current` — The Boot Initialization

```asm
.macro scs_load_current
#ifdef CONFIG_SHADOW_CALL_STACK
    // x18 = &init_task (from sp_el0 set in OP1)
    mrs     x18, sp_el0
    // x18 = init_task.thread_info.scs_sp (the pre-allocated SCS pointer)
    ldr     x18, [x18, #TSK_TI_SCS_SP]
#endif
.endm
```

`init_task.thread_info.scs_sp` is initialized at compile time to point to the
base of `init_task`'s shadow call stack — a separate buffer allocated in `.bss`
or `.data`.

After `scs_load_current`:
- `x18` points to the shadow call stack for `init_task`
- The first function call (`start_kernel` via `bl`) will push its return address
  to `[x18]` and advance `x18`

---

## Memory Layout with SCS

```
init_task kernel stack (16KB):
┌─────────────────────────────────┐
│  [pt_regs reservation]          │
│  [start_kernel frame]           │  x18 shadow stack NOT here
│  [setup_arch frame]             │
│  ...                            │
└─────────────────────────────────┘

init_task shadow call stack (separate region, 4KB):
┌─────────────────────────────────┐  ← initial x18
│  [return addr: start_kernel]    │  ← x18 after first bl
│  [return addr: setup_arch]      │  ← x18 after second bl
│  [return addr: ...]             │
└─────────────────────────────────┘

Regular stack:  used for local vars, saved registers, arguments
Shadow stack:   used ONLY for return addresses
```

The separation means an overflow of the regular stack CANNOT reach shadow stack
return addresses. An attacker would need to corrupt `x18` itself — but `x18` is
a CPU register, not memory. It can only be changed by explicitly executing
`msr`/`mov x18` instructions, which the kernel never does (outside of context switch).

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