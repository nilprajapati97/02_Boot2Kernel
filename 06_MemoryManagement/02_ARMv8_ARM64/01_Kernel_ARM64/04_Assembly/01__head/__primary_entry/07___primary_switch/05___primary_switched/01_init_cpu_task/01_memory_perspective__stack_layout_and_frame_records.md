# init_cpu_task — Memory Perspective: Stack Layout, pt_regs, and Frame Records

**Classification**: ARM64 Kernel Memory Architecture — Boot Sequence
**Scope**: Kernel stack initialization, unwinder sentinel, frame record protocol
**Perspective**: Virtual memory layout, byte-level stack structure
**Style Reference**: Google System Architecture / NVIDIA Memory Subsystem TRM

---

## 1. The Problem: How Does the Stack Unwinder Know When to Stop?

When `panic()` prints a backtrace, it walks a chain of frame records:
```
  current_frame.fp → caller_frame.fp → caller's_caller.fp → ...
```

Eventually, FP must point to something that says "stop here". Without a
defined terminator, the unwinder would follow a zero or garbage FP into
unmapped memory, triggering another fault inside the fault handler — a fatal
double fault.

`init_cpu_task` solves this by constructing a **synthetic bottom frame record**
with a special type that the unwinder recognises as the terminal node.

---

## 2. ARM64 Frame Record Format

ARM64 AAPCS64 defines a **standard frame record** as two consecutive 8-byte
values pushed to the stack by every non-leaf function:

```
  High address
  ┌──────────────┐
  │      LR      │  [fp + 8]  return address (link register)
  ├──────────────┤
  │      FP      │  [fp + 0]  ← frame pointer (x29) points HERE
  └──────────────┘             points to caller's frame record
  Low address
```

Walking the call stack = following `fp` → `fp[0]` → `fp[0][0]` → ...

To terminate this chain safely, Linux introduces the **metadata frame record**:

```c
// arch/arm64/include/asm/stacktrace/frame.h
#define FRAME_META_TYPE_NONE        0   // reserved
#define FRAME_META_TYPE_FINAL       1   // bottom of stack — stop successfully
#define FRAME_META_TYPE_PT_REGS     2   // embedded pt_regs — follow pc then lr
```

A metadata frame record is a special frame where `{fp=0, lr=0}` followed
immediately by a `type` field. The zero fp/lr pair is the detection signal.

---

## 3. Byte Layout of `init_task` Kernel Stack After `init_cpu_task`

```
Virtual Address Space (init_task.stack)
═══════════════════════════════════════════════════════════════════════════

  init_task.stack + THREAD_SIZE (16384 = 0x4000)   HIGH ADDR ↑
  ┌─────────────────────────────────────────────────────────┐
  │                   pt_regs region                        │  PT_REGS_SIZE = 336 bytes
  │                                                         │
  │  Offset from pt_regs base (struct pt_regs layout):      │
  │  ├── [+0x000]  regs[0..29]   (x0..x29)  240 bytes       │
  │  ├── [+0x0F0]  sp            8 bytes                    │
  │  ├── [+0x0F8]  pc            8 bytes                    │
  │  ├── [+0x100]  pstate        8 bytes                    │
  │  ├── [+0x108]  orig_x0       8 bytes                    │
  │  ├── [+0x110]  syscallno     8 bytes                    │
  │  ├── [+0x118]  sdei_ttbr1    8 bytes  (or pmr_save)     │
  │  ├── [+0x120]  stackframe    ← S_STACKFRAME              │
  │  │     ├── [+0x120] fp = 0  ← stp xzr, xzr             │
  │  │     ├── [+0x128] lr = 0  ← stp xzr, xzr             │
  │  │     └── [+0x130] type = 1 (FRAME_META_TYPE_FINAL)    │
  │  └──────────────────────────────────────────────────────│
  │                                                         │
  │  sp = init_task.stack + THREAD_SIZE - PT_REGS_SIZE      │
  │     = init_task.stack + 0x4000 - 0x150                  │
  │     = init_task.stack + 0x3EB0                          │
  └─────────────────────────────────────────────────────────┘
         ← sp points here after init_cpu_task
  ┌─────────────────────────────────────────────────────────┐
  │                                                         │
  │           Kernel stack (grows DOWN)                     │
  │                                                         │
  │    ← start_kernel() frames will be pushed here          │
  │    ← each bl/blr saves {fp, lr} here                    │
  │                                                         │
  │                                                         │
  │                                                         │
  └─────────────────────────────────────────────────────────┘
  init_task.stack + 0x0000                      LOW ADDR ↓

═══════════════════════════════════════════════════════════════════════════
```

### Computed Values (typical 4KB page, THREAD_SHIFT=14)

```
THREAD_SIZE      = 1 << THREAD_SHIFT = 16384 = 0x4000
PT_REGS_SIZE     = sizeof(struct pt_regs) = 336 = 0x150

sp after init_cpu_task = init_task.stack + 0x4000 - 0x150
                       = init_task.stack + 0x3EB0
```

---

## 4. The FINAL Frame Record — Construction Step by Step

### Step 1: Write zero {fp, lr} at S_STACKFRAME

```asm
stp  xzr, xzr, [sp, #S_STACKFRAME]
```

`S_STACKFRAME` is the compile-time offset of `pt_regs.stackframe.fp` from
the base of `struct pt_regs`. Storing `{0, 0}` creates the detection pattern:
"fp=0 and lr=0" signals a metadata frame record to the unwinder.

### Step 2: Store type = FRAME_META_TYPE_FINAL

```asm
mov  tmp1, #FRAME_META_TYPE_FINAL    // = 1
str  tmp1, [sp, #S_STACKFRAME_TYPE]
```

`S_STACKFRAME_TYPE` = `S_STACKFRAME + 16` (the type field follows {fp, lr}).
This declares the type as `1 = FINAL`, meaning: "unwinding terminates
successfully here — no error."

### Step 3: Set x29 (FP) to point at this record

```asm
add  x29, sp, #S_STACKFRAME
```

After this, x29 (the frame pointer) points directly at the synthetic bottom
frame. All subsequent frames pushed by `start_kernel` and its callees will
form a chain that eventually links back to this record.

---

## 5. Unwinder Walk — How a Panic Backtrace Terminates Correctly

```
                    Kernel panic in some_deep_function():

  some_deep_function   fp → ┌──────────────────────────────────┐
                            │ lr: some_deep_function's caller  │
                            │ fp: ──────────────────────────── │ → next frame
                            └──────────────────────────────────┘
                                          ...
                                          │
                        several frames up │
                                          ↓
  start_kernel          fp → ┌──────────────────────────────────┐
                            │ lr: (return to __primary_switched)│
                            │ fp: ──────────────────────────── │ → FINAL frame
                            └──────────────────────────────────┘
                                          │
                                          ↓
  FINAL frame sentinel  fp → ┌──────────────────────────────────┐
                            │ fp: 0                            │ ← zero → metadata
                            │ lr: 0                            │
                            │ type: 1 (FRAME_META_TYPE_FINAL)  │
                            └──────────────────────────────────┘
                                          │
                                          ↓
                         Unwinder reads type=1 → STOP. Print "End of trace."
                         NO fault, NO "corrupted frame" message.
```

Without the FINAL sentinel, what would happen:
```
  start_kernel frame:  fp = &FINAL_sentinel
  FINAL sentinel:      fp = 0
  Unwinder reads fp=0 → attempts to read [0+0] → maps to kernel VA 0x0
                      → swapper_pg_dir has no mapping at VA 0
                      → Translation fault during fault handler
                      → Double fault → system reset or hang
```

---

## 6. VMAP_STACK: Stack Overflow Detection

If `CONFIG_VMAP_STACK` is enabled, each kernel stack is backed by **virtual
memory** with a **guard page** below the bottom of the stack:

```
  init_task.stack + THREAD_SIZE    ← top (pt_regs)
  init_task.stack + PAGE_SIZE      ← bottom of usable stack
  init_task.stack + 0              ← guard page (unmapped)
```

When the stack overflows past `init_task.stack + PAGE_SIZE`, the CPU
attempts to write to the guard page, triggering a translation fault.
The fault handler detects this specifically (by checking if SP is in a guard
page region) and prints "Kernel stack overflow" rather than a generic fault.

The condition is checked via:
```asm
// exception.S kernel_entry macro
tbnz  sp, #THREAD_SHIFT, overflow_stack
```

If `sp & (1 << THREAD_SHIFT)` is non-zero, the SP has gone below the
`THREAD_SIZE`-aligned base and we've overflowed.

---

## 7. Shadow Call Stack (SCS) — Second Stack for Return Addresses

```
CONFIG_SHADOW_CALL_STACK:

  Regular stack (SP / SP_EL1):           Shadow Call Stack (x18):
  ┌──────────────────────────┐           ┌──────────────────────────┐
  │  local variables         │           │  return_addr_N           │
  │  callee-saved regs       │           │  return_addr_N-1         │
  │  fp (frame pointer)      │           │  ...                     │
  │  lr (return address) ← ─ ─ ─ ─COPY─ ─│  return_addr_1           │
  └──────────────────────────┘           └──────────────────────────┘
           ↑                                         ↑
     vulnerable to                           separate virtual page,
     buffer overflow                          not adjacent to stack
```

`scs_load_current` (in `init_cpu_task`):
```asm
ldr  x18, [x4, #TSK_TI_SCS_BASE]    // x18 = init_task.scs_base
```

For `init_task`, the SCS base is statically allocated in `.init.data`.
For all subsequent tasks created by `fork`, the SCS region is dynamically
allocated from a dedicated pool by `arch_dup_task_struct`.

The SCS provides **return address integrity** — an attacker who corrupts a
stack buffer cannot overwrite the return address stored in the SCS because
it resides at a different virtual address, protected by page table permissions.

---

## 8. Memory Ordering: Why No Barriers in `init_cpu_task`?

```
msr  sp_el0, x4          (no barrier needed)
ldr  tmp1, [x4, #TSK_STACK]
add  sp,  tmp1, #THREAD_SIZE
sub  sp,  sp,  #PT_REGS_SIZE    (no barrier needed)
stp  xzr, xzr, [sp, #S_STACKFRAME]
str  tmp1, [sp, #S_STACKFRAME_TYPE]
add  x29, sp, #S_STACKFRAME
msr  tpidr_el1, tmp1     (no barrier needed)
```

No barriers (DSB/ISB) are required within `init_cpu_task` because:

1. **All operations are local to the current CPU** — no other CPU observes
   these registers or the early stack (CPUs are not yet online)
2. **System register writes** (`msr sp_el0`, `msr tpidr_el1`) have
   **immediate effect** for non-architectural registers — they are not
   context-synchronizing events that require ISB
3. **Stack writes** (`stp`, `str`) are normal memory stores — they complete
   in program order relative to subsequent reads on the same CPU
4. The **only register requiring ISB** in the entire sequence is `vbar_el1`,
   which is written **after** `init_cpu_task` returns, in the next step
   of `__primary_switched`

**Google Security Note**: The absence of barriers here is NOT a bug. Adding
unnecessary DSB/ISB would add latency without providing any correctness
benefit, and would obscure the meaningful barrier at `msr vbar_el1` + `isb`.
