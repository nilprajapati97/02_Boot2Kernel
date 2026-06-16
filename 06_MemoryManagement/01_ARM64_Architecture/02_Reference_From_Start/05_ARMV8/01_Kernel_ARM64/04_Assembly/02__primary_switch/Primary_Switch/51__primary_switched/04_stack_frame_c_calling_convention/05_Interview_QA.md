# Stack Frame and Calling Convention — Interview Q&A

---

## Q1: What does `stp x29, x30, [sp, #-16]!` do? Explain every part.

**A:**
- `stp` = Store Pair: stores two 64-bit registers to memory in one instruction.
- `x29` = first register to store = frame pointer.
- `x30` = second register to store = link register (return address).
- `[sp, #-16]!` = pre-indexed addressing:
  - `!` = write-back: `sp` is modified
  - `sp = sp - 16` FIRST, then memory access uses updated sp
  - `[sp + 0] = x29`, `[sp + 8] = x30`
- Net effect: allocates 16 bytes on the stack and stores the frame pointer and
  return address, creating a standard C function frame record.

---

## Q2: Why does AAPCS64 require 16-byte stack alignment at function calls?

**A:** ARM64 NEON/SIMD instructions (e.g., `stp q0, q1, [sp]`) require 16-byte
alignment for proper operation. Also, the architecture's `ldp`/`stp` pair
instructions with SP as base register will generate an alignment fault if SP
is not 16-byte aligned (when SCTLR_EL1.SA = 1, stack alignment checking enabled).
The 16-byte rule ensures all SIMD loads/stores from the stack are naturally aligned.

---

## Q3: What is the frame pointer used for and what breaks if it's wrong?

**A:** The frame pointer (`x29`) is used for:
1. **Stack unwinding**: debuggers and profilers (GDB, perf, ftrace) walk the FP
   chain to generate callstacks.
2. **KASAN/KCSAN stack traces**: sanitizers dump call stacks using FP.
3. **Kernel panic output**: `dump_stack()` uses FP to walk frames.

If `x29` is wrong:
- Stack traces in panic output show garbage or stop early
- `perf --call-graph fp` shows incorrect call graphs
- KASAN errors lack proper call sites, making bugs hard to find

---

## Q4: `start_kernel` is marked `asmlinkage`. What does that mean on ARM64?

**A:** `asmlinkage` (defined as `__attribute__((visibility("default")))` on ARM64, or
sometimes empty) indicates the function is called from assembly. On x86, it means
"pass all arguments on the stack". On ARM64, the calling convention already uses
registers (x0–x7) for arguments, so `asmlinkage` primarily means the function has
external linkage and won't have its calling convention changed by LTO optimizations.
`start_kernel` takes no arguments, so it makes no practical difference on ARM64 —
it's there for documentation and x86 compatibility.

---

## Q5: Why does `start_kernel` use `bl` and not `b`?

**A:** `b` (Branch) is a tail-call: the CPU jumps to `start_kernel` without setting
`x30`, so `start_kernel`'s frame chain would have no link back to `__primary_switched`.
`bl` (Branch with Link) sets `x30 = PC+4` before jumping, creating a valid return
address. This is important for:
1. The stack unwinder can correctly trace `start_kernel` → `__primary_switched`
2. `start_kernel` can theoretically return (even though it never does)
3. Crash traces show "called from `__primary_switched`" correctly

Using `b` would make the boot call graph appear truncated at `start_kernel`.

---

## Q6: How does `-fno-omit-frame-pointer` affect code generation?

**A:** Without this flag, the compiler is free to use `x29` as a general-purpose
register (especially in leaf functions) for optimization. With `-fno-omit-frame-pointer`:
- Every non-leaf function must execute `stp x29, x30, [sp, #-16]!; mov x29, sp`
  in its prologue
- Every non-leaf function must execute `ldp x29, x30, [sp], #16` in its epilogue
- `x29` is reserved for the frame pointer throughout each function

Cost: ~2 extra instructions per function call + 16 bytes of stack per frame.
Benefit: Always-valid FP chain enables reliable debugging, profiling, and crash analysis.

---

## Q7: What is the "red zone" and does ARM64 Linux use it?

**A:** The "red zone" is a 128-byte area BELOW the current stack pointer that the
System V ABI (x86-64) guarantees is not clobbered by signal delivery or interrupt
handlers. Leaf functions can store temporaries in the red zone without adjusting sp.

ARM64 Linux does NOT use a red zone (it's not part of AAPCS64). Signal delivery
on ARM64 always adjusts sp before using the stack (using `__put_user` to check
the new sp is valid). The kernel's own interrupt handlers (exception entry code)
always start with `sub sp, sp, #PT_REGS_SIZE` to allocate properly. No ARM64 code
relies on a red zone.

---

## Q8: If `start_kernel` panics early, what does the stack trace show?

**A:** The stack trace would be something like:
```
Kernel panic - not syncing: ...
CPU: 0 PID: 1 Comm: swapper Not tainted
Hardware name: ...
Call trace:
 dump_backtrace+0x0/0x...
 show_stack+0x...
 dump_stack+0x...
 panic+0x...
 start_kernel+0x...    ← the crash site
 __primary_switched+0x5c ← our bl start_kernel instruction
```

The `__primary_switched+0x5c` frame is visible because:
1. `stp x29, x30, [sp, #-16]!` saved x30 = return address inside __primary_switched
2. `start_kernel` saved x29/x30 in its own prologue
3. The FP chain terminates at the sentinel `x29=0` set by `init_cpu_task`
4. `dump_backtrace()` stops when it sees `x29 = 0`

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