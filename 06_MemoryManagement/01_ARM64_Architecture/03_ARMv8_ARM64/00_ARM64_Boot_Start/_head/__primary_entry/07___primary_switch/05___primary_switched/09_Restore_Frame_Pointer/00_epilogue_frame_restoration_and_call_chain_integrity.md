# Restore Frame Pointer — Epilogue, ABI Completion, and Call Chain Integrity

**File**: `arch/arm64/kernel/head.S` — inside `__primary_switched`
**Instructions**:
```asm
ldp     x29, x30, [sp], #16    // Pop {FP, LR} — standard function epilogue
```
**Perspective**: ARM64 ABI / Stack Unwinder Correctness
**Style**: Google AAPCS64 Reference / AMD Debugging Engineering

---

## 1. The Epilogue Paired With the Prologue

In `03_Establish_Calling_Frame`, the prologue saved FP and LR:
```asm
stp   x29, x30, [sp, #-16]!   // prologue: push {FP, LR}, SP -= 16
mov   x29, sp                  // FP = SP
```

This epilogue is the exact mirror:
```asm
ldp   x29, x30, [sp], #16     // pop {FP, LR}, SP += 16
```

After `ldp`:
- `x29` (FP) restored to the FINAL sentinel frame address (set by `init_cpu_task`)
- `x30` (LR) restored to the return address into `__primary_switch`
- `SP` advanced by 16, back to the value before the prologue

```
Stack state progression:
  [init_cpu_task done]:   SP → top of init_task stack
  [after prologue]:       SP → top - 16  (frame record allocated)
  [after epilogue]:       SP → top of init_task stack  (frame record freed)
                          x29 = FINAL sentinel address  ← restored
```

---

## 2. Why Restore Before `bl start_kernel` (Not After)

`start_kernel` never returns. The `bl` instruction will branch to
`start_kernel` and the kernel runs forever. The sequence is:

```asm
ldp   x29, x30, [sp], #16   // ← epilogue
bl    start_kernel           // ← calls start_kernel with current SP, x29, x30
ASM_BUG()                    // ← unreachable
```

**The epilogue fires BEFORE the `bl`**, restoring x29 so that `start_kernel`
sees the correct caller frame pointer at its entry point.

Inside `start_kernel`, the very first thing the compiler generates is:
```asm
// start_kernel compiled prologue:
stp   x29, x30, [sp, #-N]!
mov   x29, sp
```

This saves `x29` (which = FINAL sentinel address, freshly restored) and
`x30` (= address of `ASM_BUG()` in `__primary_switched`).

If the epilogue had **not** been done, `start_kernel`'s frame record would save
a stale x29 pointing into the middle of `__primary_switched`'s frame, which
is now garbage (SP has moved). The call chain would be broken.

---

## 3. What `x30` Points To After Restore

After the `ldp` restores `x30`, it contains the **original return address**
of `__primary_switched` — which is the instruction in `__primary_switch`
right after `br x8`:

```asm
// __primary_switch:
    adrp    x8, TRAMP_VALIAS
    // ...
    br      x8              // branch to __primary_switched
NEXT_INSN:                  // ← x30 value after restore
    // (but this code is never reached)
```

So `x30` = `NEXT_INSN` address. This is never actually executed because:
1. `bl start_kernel` overwrites x30 with the address of `ASM_BUG()`
2. `start_kernel` never returns
3. Even if it did, it would return to `ASM_BUG()` which traps

The value in x30 after the epilogue is **meaningful for backtraces** —
a stack unwinder walking the chain will correctly see `__primary_switch`
as the caller of `__primary_switched` — but is never actually used as a
branch target.

---

## 4. Post-Increment Load Pair Semantics

```asm
ldp   x29, x30, [sp], #16

Equivalent to:
  ldr  x29, [sp, #0]       // load FP from [sp+0]
  ldr  x30, [sp, #8]       // load LR from [sp+8]
  add  sp, sp, #16         // SP += 16 (post-increment)
```

The `#16` post-increment exactly undoes the `#-16` pre-decrement from the
prologue. SP is 16-byte aligned before the epilogue (AAPCS64 requirement)
and remains 16-byte aligned after.

Compare with the prologue's `[sp, #-16]!` (pre-decrement / writeback):
- **Pre-decrement** (`stp ... [sp, #-N]!`): allocate then store
- **Post-increment** (`ldp ... [sp], #N`): load then deallocate

These two patterns are the canonical ARM64 function frame push/pop idiom
used throughout the Linux kernel and all ARM64 software.

---

## 5. AMD-Style State Snapshot: Before vs After Epilogue

```
Register and Stack State:
┌──────────────────────────────────────────────────────────────────────────┐
│              BEFORE epilogue             │    AFTER epilogue              │
├──────────────┬───────────────────────────┼────────────────────────────────┤
│  SP          │  top - 16                 │  top (init_task stack top)     │
├──────────────┼───────────────────────────┼────────────────────────────────┤
│  x29 (FP)   │  &frame record            │  FINAL sentinel addr           │
│              │  (= SP before epilogue)   │  (= old x29 from prologue)     │
├──────────────┼───────────────────────────┼────────────────────────────────┤
│  x30 (LR)   │  return addr of           │  return addr of                │
│              │  __primary_switched       │  __primary_switched            │
│              │  (= `bl start_kernel`     │  SAME VALUE — restored from    │
│              │   just overwrote it?)     │  the frame record              │
├──────────────┼───────────────────────────┼────────────────────────────────┤
│  [SP+0]     │  old x29 (valid)          │  (deallocated)                 │
├──────────────┼───────────────────────────┼────────────────────────────────┤
│  [SP+8]     │  old x30 (valid)          │  (deallocated)                 │
└──────────────┴───────────────────────────┴────────────────────────────────┘

Note: x30 is the same before and after only because bl start_kernel hasn't
fired yet. The epilogue restores it from the saved frame record — which has
the same value as the current x30 in register (since no bl happened between
the prologue and now that wasn't correctly matched with its own prologue).
```

---

## 6. Unwinder Chain After Epilogue, Before `bl start_kernel`

```
At this exact moment (after epilogue, before bl start_kernel):

x29 → [FINAL sentinel: fp=0, lr=0, type=FINAL]
                ↑
SP → (no frame record allocated, SP at clean stack top)

When bl start_kernel fires:
  x30 ← ASM_BUG() address    (return address for start_kernel)
  start_kernel prologue:
    stp x29, x30, [sp, #-N]!  → saves {FINAL sentinel addr, ASM_BUG() addr}
    mov x29, sp               → FP = start_kernel's frame

Now the chain is:
  start_kernel frame → [x29: FINAL sentinel]
  FINAL sentinel     → [fp=0, lr=0] = end of trace

Any panic inside start_kernel will show:
  start_kernel
  [end of trace]     (FINAL sentinel terminates the walk)
```

The epilogue here is what connects `start_kernel`'s frame record back to
the FINAL sentinel — completing the call chain correctly for all future
stack traces from within the C kernel.
