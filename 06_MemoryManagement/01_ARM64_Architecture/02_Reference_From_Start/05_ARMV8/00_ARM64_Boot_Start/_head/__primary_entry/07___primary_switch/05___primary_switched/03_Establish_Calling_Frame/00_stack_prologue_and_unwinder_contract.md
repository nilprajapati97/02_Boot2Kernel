# Establish Calling Frame — Stack Prologue, LR, and the Unwinder Contract

**File**: `arch/arm64/kernel/head.S` — inside `__primary_switched`
**Instructions**:
```asm
stp     x29, x30, [sp, #-16]!   // Push {FP, LR} — standard function prologue
mov     x29, sp                  // Update frame pointer to new frame
```
**Perspective**: ARM64 ABI / Stack Unwinder Architecture
**Style**: Google Android / ARM AAPCS64 Reference

---

## 1. The Standard ARM64 Function Prologue

Every non-leaf ARM64 function that calls other functions must save the
**frame pointer (x29)** and **link register (x30 / LR)** at entry.
This is the AAPCS64 (ARM64 ABI) requirement that enables stack unwinding.

```asm
stp   x29, x30, [sp, #-16]!    // pre-decrement SP by 16, store {FP, LR}
mov   x29, sp                  // FP = new SP (points at this frame record)
```

After these two instructions, the stack looks like:

```
  ← SP before prologue
  ┌──────────────────────────────────┐
  │  old x30 (LR)  [sp + 8]         │   return address
  ├──────────────────────────────────┤
  │  old x29 (FP)  [sp + 0]  ← SP   │   previous frame pointer
  └──────────────────────────────────┘
  ← SP after prologue  (= x29 now)
```

`x29` (FP) now points at this 16-byte frame record. The `old x29` field
links back to the **caller's frame record**, forming the call chain.

---

## 2. Why `__primary_switched` Needs a Prologue at All

`__primary_switched` never returns — it calls `start_kernel` and the kernel
runs forever. So why save LR?

**Reason 1: Stack unwinder requires a complete chain.**

`init_cpu_task` installed a FINAL frame sentinel at `pt_regs.stackframe`.
After `init_cpu_task`, `x29` points at that sentinel. The prologue here
creates a new frame **on top of** the sentinel:

```
After init_cpu_task:
  x29 → FINAL sentinel {fp=0, lr=0, type=FINAL}

After this prologue:
  x29 → ┌──────────────────────────────────┐  ← NEW FRAME
        │  old x29 (= FINAL sentinel addr) │  → links to FINAL sentinel
        ├──────────────────────────────────┤
        │  old x30 (= LR = return to ???)  │
        └──────────────────────────────────┘
```

Any panic backtrace from inside `start_kernel` → `rest_init` → any deep
call will eventually chain back to:
```
  <some_function>
  start_kernel
  __primary_switched  ← this frame
  [end of trace]      ← FINAL sentinel
```

Without this prologue, the last valid frame would be the FINAL sentinel
directly, and the backtrace would show `start_kernel` with no calling context.

**Reason 2: LR is a callee-saved register — callees may clobber it.**

`bl init_cpu_task` (a macro) and subsequent `bl` calls use x30 for their
own return addresses. Without saving x30 here, the return address of
`__primary_switched` itself (needed for consistent backtraces) would be lost.

---

## 3. The `pre-decrement` Store Pair (`stp ... [sp, #-16]!`)

The `!` suffix in ARM64 assembly means **writeback** — the base register
(`sp`) is updated after the store:

```
stp  x29, x30, [sp, #-16]!

Equivalent to:
  str  x29, [sp, #-16]    // store FP at sp - 16
  str  x30, [sp, #-8]     // store LR at sp - 8
  sub  sp, sp, #16        // sp = sp - 16 (update SP)

Atomic in terms of stack state — no window where sp points
to uninitialized memory between the stores.
```

**Stack alignment**: The allocation is 16 bytes → SP remains 16-byte aligned
after the prologue. AAPCS64 requires SP to be 16-byte aligned at all `bl`
call sites. ✓

---

## 4. Matching Epilogue and Why It Matters for `start_kernel`

The matching epilogue appears just before `bl start_kernel`:

```asm
ldp   x29, x30, [sp], #16    // Pop {FP, LR} — epilogue

bl    start_kernel            // Enter C kernel — no return
ASM_BUG()                     // Unreachable
```

The `ldp ... [sp], #16` (post-increment) restores x29 and x30 and advances SP.
This restores the **caller's** frame pointer so that if `start_kernel`
ever returns (it shouldn't), the chain is valid.

The fact that `bl start_kernel` immediately follows the epilogue (without
a `ret`) means `start_kernel` is called as a **tail call** from the
perspective of the frame chain — but with a valid return address in x30
pointing to `ASM_BUG()`.

---

## 5. AMD-Style Register State Snapshot: Before vs After Prologue

```
Register State at prologue:
┌────────────────────────────────────────────────────────────────────────┐
│             BEFORE prologue          │    AFTER prologue               │
├─────────────┬────────────────────────┼─────────────────────────────────┤
│  SP         │  init_task stack top  │  init_task stack top - 16       │
│             │  (post init_cpu_task) │                                  │
├─────────────┼────────────────────────┼─────────────────────────────────┤
│  x29 (FP)   │  FINAL sentinel addr  │  SP (new frame addr)            │
├─────────────┼────────────────────────┼─────────────────────────────────┤
│  x30 (LR)   │  return addr (to ???) │  UNCHANGED (still in register)  │
│             │  (never used anyway)  │  but saved to [SP+8]            │
├─────────────┼────────────────────────┼─────────────────────────────────┤
│  [SP+0]     │  (unallocated)        │  old x29 (FINAL sentinel addr)  │
├─────────────┼────────────────────────┼─────────────────────────────────┤
│  [SP+8]     │  (unallocated)        │  old x30 (return address)       │
└─────────────┴────────────────────────┴─────────────────────────────────┘
```

---

## 6. Resulting Call Chain Visible in Panic Backtraces

After the prologue, a kernel panic from anywhere in `start_kernel` will
produce a backtrace like:

```
Call trace:
  dump_backtrace+0x...
  show_stack+0x...
  dump_stack_lvl+0x...
  panic+0x...
  ...
  start_kernel+0x...
  0x0                   ← __primary_switched frame (shows as 0x0 since
                          no debug symbol — or as "__primary_switched" with
                          CONFIG_KALLSYMS)
```

The `[end of trace]` line appears only when the unwinder hits the
FINAL sentinel. Without the prologue in `__primary_switched`, the
`start_kernel` frame would appear to have no caller — the unwinder would
prematurely terminate or print a spurious "corrupted stack" warning.
