# Stack Unwinding — How GDB and perf Trace Through Boot Frames

## Frame Pointer Unwinding on ARM64

ARM64 Linux is compiled with `-fno-omit-frame-pointer` for kernel code (when
`CONFIG_FRAME_POINTER=y`, which is the default). This means every C function:
1. Saves `{x29, x30}` on the stack
2. Sets `x29 = sp` (frame pointer = base of current frame)
3. Does its work
4. Restores `{x29, x30}` and returns

This creates a linked list of frames in memory.

---

## The Unwind Chain from `start_kernel` to Boot

When a bug occurs deep in `start_kernel` initialization, the stack trace looks like:

```
Stack trace (FP chain):
  [0] crash_function+0x24 (fp=0xffff800009xxxx00)
  [1] some_init_function+0x48 (fp=0xffff800009xxxx30)
  [2] start_kernel+0x1c0 (fp=0xffff800009xxxx80)
  [3] __primary_switched+0x5c (fp=0x0000000000000000)
  ↑ chain terminates: x29 = 0
```

Step-by-step FP chain traversal:
```
1. Current x29 = 0xffff800009xxxx00
   Load: [x29+0] = 0xffff800009xxxx30 (previous frame's x29)
         [x29+8] = some_init_function + return_offset (previous frame's x30)
   
2. x29 = 0xffff800009xxxx30
   Load: [x29+0] = 0xffff800009xxxx80 (start_kernel's frame)
         [x29+8] = some_init_function + call_site_offset

3. x29 = 0xffff800009xxxx80 (start_kernel's frame)
   Load: [x29+0] = __primary_switched frame
         [x29+8] = start_kernel return address (never used)

4. x29 = __primary_switched frame (from stp x29, x30 / mov x29, sp)
   Load: [x29+0] = 0  (the sentinel x29 from init_cpu_task)
         [x29+8] = ??  (the original x30 at entry to __primary_switched)

5. x29 = 0 → STOP: NULL frame pointer terminates the chain
```

---

## The Sentinel Frame — Set by `init_cpu_task`

`init_cpu_task` (in `arch/arm64/kernel/asm-offsets.c`-defined offsets) sets:
```asm
stp   xzr, xzr, [sp, #S_STACKFRAME]   // [sp + S_STACKFRAME] = {0, 0}
add   x29, sp, #S_STACKFRAME           // x29 = &that sentinel pair
```

This creates:
```
sp + S_STACKFRAME + 0:  0x0000000000000000  ← saved x29 (fp=0 = chain terminator)
sp + S_STACKFRAME + 8:  0x0000000000000000  ← saved x30 (lr=0 = "no caller")
```

Then `__primary_switched` creates another frame ON TOP of this:
```asm
stp   x29, x30, [sp, #-16]!   // new frame: saved x29 = S_STACKFRAME addr, saved x30 = real lr
mov   x29, sp
```

So the final chain is:
```
__primary_switched frame (x29 at stp point):
  [+0] = S_STACKFRAME addr    (the sentinel frame pointer)
  [+8] = original lr          (whatever called __primary_switched)
     │
     ▼
Sentinel frame (at sp + S_STACKFRAME):
  [+0] = 0    ← CHAIN TERMINATES HERE
  [+8] = 0
```

---

## `perf record --call-graph fp` Usage

To capture boot-time callgraphs with frame pointers:
```bash
# Boot with CONFIG_FRAME_POINTER=y (default for ARM64)
# Then after boot:
perf record --call-graph fp -g sleep 5
perf report --call-graph

# For kernel boot tracing (with perf_events on early ring buffer):
perf record -e cycles:k --call-graph fp -a -- sleep 5
```

If the frame pointer chain is intact (which our `stp x29, x30 / mov x29, sp` setup
ensures), perf can show complete kernel callgraphs.

---

## DWARF vs Frame Pointer Unwinding

ARM64 Linux supports both:

| Method | How | Accuracy | Overhead |
|---|---|---|---|
| Frame pointer | FP chain walk | 100% for kernel | Compile-time: 1 extra reg, 1 extra instr per function |
| DWARF | .eh_frame/.debug_frame | 100% but slow | Per-frame metadata; expensive to parse |
| ORC (Linux-specific) | arch/arm64: not used | N/A | N/A |

ARM64 Linux kernel uses frame pointers (not ORC, which is x86-only). The
`stp x29, x30` in `__primary_switched` is MANDATORY for correct frame-pointer
unwinding through the earliest kernel code.

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