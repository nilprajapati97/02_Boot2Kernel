# AAPCS64 Stack Alignment Rules and Why They Matter

## The 16-Byte Alignment Requirement

AAPCS64 §6.2.2 states:
> "The stack pointer must be aligned to a 16-byte boundary at any **public interface**
> — including at the point of a function call (`bl`/`blr`)."

"Public interface" means: at the `bl` instruction itself, `sp` mod 16 must == 0.

---

## Stack State Through `__primary_switched`

```
1. Enter __primary_switched:
   init_stack top = init_stack + THREAD_SIZE
   
   After init_cpu_task sets sp to init_stack top:
   sp = &init_stack + THREAD_SIZE - sizeof(struct pt_regs)
      = init_stack_base + 16384 - 336          (on typical arm64)
   The THREAD_SIZE and PT_REGS reservation ensure sp is 16-byte aligned here.

2. init_cpu_task also does:
   stp  xzr, xzr, [sp, #S_STACKFRAME]    // store sentinel (16-byte write — aligned ✓)
   add  x29, sp, #S_STACKFRAME            // x29 = sentinel frame address

3. __primary_switched then does:
   stp  x29, x30, [sp, #-16]!             // sp -= 16 (still aligned ✓)
   mov  x29, sp                            // x29 = new sp

4. Additional register saves (if any):
   (none before bl start_kernel in typical config)

5. bl start_kernel:
   sp is 16-byte aligned ✓ ← AAPCS64 requirement satisfied
```

---

## The `stp` Instruction vs Manual `sub sp`

Two equivalent approaches to creating a frame:

**Approach A (Linux uses this):**
```asm
stp  x29, x30, [sp, #-16]!   // atomic: allocate + store
mov  x29, sp
```

**Approach B (also valid):**
```asm
sub  sp, sp, #16              // allocate first
stp  x29, x30, [sp]           // then store
mov  x29, sp
```

Approach A is preferred because:
1. One instruction instead of two (fewer cycles)
2. The pre-indexed addressing mode is atomic (single micro-op on most CPUs)
3. Cannot generate a misaligned access (hardware guarantees `[sp, #-16]` is 16-byte aligned if sp was)
4. More idiomatic — recognized by tools like GDB, perf, and the kernel's own unwinder

---

## x29 as the Frame Pointer

`x29` is designated as the frame pointer by AAPCS64. The significance:

```
Stack layout after bl start_kernel enters:
                            ┌──────────────────────────────┐
  __primary_switched        │  x29 (old, = sentinel addr)  │ ← sp_before_stp + 0
  frame starts here ──►sp   │  x30 (lr to callee of pri..) │ ← sp_before_stp + 8
                            ├──────────────────────────────┤
                            │  start_kernel's x29          │ ← sp_before_stp - 16 + 0
  start_kernel              │  start_kernel's x30          │ ← sp_before_stp - 16 + 8
  frame starts here ──►sp   ├──────────────────────────────┤
                            │  start_kernel's local vars   │
                            │  ...                         │
                            └──────────────────────────────┘
```

The frame pointer chain:
```
start_kernel.x29 → __primary_switched frame → sentinel frame → x29=0 (end)
```

Debugger (GDB) and `perf record --call-graph fp` use this chain for call stacks.

---

## Why `mov x29, sp` and Not Something Else?

After `stp x29, x30, [sp, #-16]!`, `sp` points to the newly allocated frame
(the slot where we stored old `x29`). Setting `x29 = sp` means:
- x29 points to [saved_x29, saved_x30] — the standard frame record
- Any function that accesses locals via `[x29, #offset]` works correctly
- The unwinder can load `x29` → follow to `[x29, #0]` → get previous frame

If we used `add x29, sp, #16` instead:
- x29 would point above the frame (to the caller's stack)
- Stack unwinding would be off by one frame (corrupt)
- Address sanitizers could report false frame mismatches

---

## Stack Alignment at Secondary CPU Startup

Secondary CPUs (SMP) go through `secondary_startup` → `secondary_switched`.
The same pattern appears:

```asm
// arch/arm64/kernel/head.S secondary_switched:
__secondary_switched:
    mov     x0, x20     // cpuid
    bl      set_cpu_boot_mode_flag
    ...
    adr_l   x5, vectors
    msr     vbar_el1, x5
    isb
    ...
    bl      cpu_init
    bl      preempt_disable
    ...
    mov     x29, #0     // different sentinel for secondary
    mov     x30, #0
    b       secondary_start_kernel    // NOTE: 'b' not 'bl' — no new frame
```

Secondary CPUs branch (`b`) rather than call (`bl`) to `secondary_start_kernel`,
so they don't push an extra frame. Primary CPU uses `bl start_kernel` which creates
a frame so the unwinder terminates cleanly.

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