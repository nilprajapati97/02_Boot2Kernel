# C Calling Convention — Stack Frame Setup in `__primary_switched`

## The Two Instructions

```asm
// __primary_switched (arch/arm64/kernel/head.S)
stp     x29, x30, [sp, #-16]!   // push frame pointer and link register
mov     x29, sp                  // x29 = frame pointer = current sp
```

These two instructions create the **first C-compatible stack frame** for the primary
boot CPU. Everything before this point is "pure assembly" — no C frame, no
unwinder-visible stack. After this, `bl start_kernel` is a proper C function call.

---

## ARM64 AAPCS64 Calling Convention Review

AAPCS64 = Procedure Call Standard for the Arm Architecture (64-bit).

**Register roles:**
| Register | AAPCS64 Role |
|---|---|
| x0–x7   | Parameter/result registers |
| x8      | Indirect result pointer |
| x9–x15  | Caller-saved temporaries |
| x16–x17 | Intra-procedure call scratch (IP0/IP1) |
| x18     | Platform register (SCS on Android/Linux) |
| x19–x28 | Callee-saved |
| x29     | **Frame pointer (FP)** |
| x30     | **Link register (LR) / return address** |
| sp      | Stack pointer |
| pc      | Program counter |

**Frame layout (AAPCS64):**
```
High address  ┌─────────────────┐  ← caller's sp (before call)
              │  caller's x29   │  (saved frame pointer)
              │  caller's x30   │  (saved return address)  ← sp points here after stp
Low address   └─────────────────┘  ← new sp (= new x29)
```

---

## What `stp x29, x30, [sp, #-16]!` Does

`stp` = Store Pair. The `!` suffix means PRE-INDEXED — sp is decremented BEFORE the store.

Step by step:
1. `sp = sp - 16`  (allocate 16 bytes)
2. `[sp + 0] = x29` (store old frame pointer — this is a SENTINEL frame)
3. `[sp + 8] = x30` (store return address — but this is never returned to)

At the top of `__primary_switched`:
- `x29` = 0 (zeroed by the sentinel frame setup in `init_cpu_task`)
- `x30` = return address from whoever called `__primary_switched` (irrelevant — boot never returns)

The 16-byte allocation satisfies the **AAPCS64 16-byte stack alignment requirement**
at function calls. Before `bl start_kernel`, sp must be 16-byte aligned.

---

## The Frame Chain at Boot

After `stp x29, x30, [sp, #-16]!; mov x29, sp`:

```
Memory (high = earlier in stack):
                         ┌─────────────────────────────────────────────────┐
init_stack + THREAD_SIZE │  [top of init_stack — stack grows down from here] │
                         ├───────────────┬─────────────────────────────────┤
                         │  sentinel fp=0│ (set by init_cpu_task)          │
                         │  sentinel lr=FINAL_MARKER (or 0)                │
                         ├───────────────┬─────────────────────────────────┤
sp = x29 ──────────────► │  x29_old = 0  │ (x29 at __primary_switched entry)│
                         │  x30_old = ?? │ (lr at __primary_switched entry) │
                         └─────────────────────────────────────────────────┘
                         ↓ stack grows downward ↓
```

When the stack unwinder traces through `start_kernel` frames:
1. Frame for `start_kernel`: x29 = frame pointer, x30 = some deeper call
2. ...frames for nested calls...
3. Eventually hits `__primary_switched` frame where x29 points to `{0, lr}`
4. Next frame: x29 = 0 → STOP (the sentinel `fp=0` terminates the chain)

---

## Why This Frame Is Needed — The `start_kernel` Contract

`start_kernel` is a C function:
```c
// init/main.c
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
{
    char *command_line;
    char *after_dashes;
    ...
```

`bl start_kernel` is a standard AAPCS64 call. On entry to `start_kernel`:
- `sp` is 16-byte aligned ✓ (guaranteed by the `stp` allocation + previous alignment)
- `x29` (fp) is set ✓ (points to the frame we just created)
- `x30` (lr) is set to `__primary_switched + offset` ✓ (the `bl` instruction sets it)

If `start_kernel` ever returns (it never should), it would return here:
- `ldp x29, x30, [sp], #16` (standard C function epilogue restores x29/x30)
- `ret` would jump to the original `x30` — which is whatever called `__primary_switched`

In practice, `start_kernel` → `rest_init` → `cpu_startup_entry(CPUHP_ONLINE)` →
idle loop. It NEVER returns.

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