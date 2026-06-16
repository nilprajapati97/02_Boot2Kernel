# init_cpu_task — ARM64 ABI Perspective: AAPCS64 Compliance and Boot ABI Contract

**Classification**: ARM64 Procedure Call Standard — Boot Path
**Scope**: ARM64 ABI requirements established by `init_cpu_task`
**Perspective**: AAPCS64, calling conventions, register allocation protocol
**Style Reference**: ARM Architecture Reference Manual / Google Android ABI Docs

---

## 1. The ARM64 Procedure Call Standard (AAPCS64): What the C Runtime Requires

Before any C function can safely execute, the CPU must satisfy the AAPCS64
(Procedure Call Standard for the Arm 64-bit Architecture) invariants.
These are not optional — violation causes undefined behavior in any C code.

### Mandatory State for C Code Entry

```
┌──────────────────────────────────────────────────────────────────────────┐
│                    AAPCS64 Requirements at C Function Entry              │
├─────────────────────┬────────────────────────────────────────────────────┤
│  Requirement        │  AAPCS64 Rule                                      │
├─────────────────────┼────────────────────────────────────────────────────┤
│  Stack pointer      │  SP must be 16-byte aligned                        │
│  alignment          │  at public interfaces                              │
├─────────────────────┼────────────────────────────────────────────────────┤
│  Frame pointer      │  x29 must point at the most recent frame record    │
│  (x29)              │  or be zero for leaf/no-unwind functions           │
├─────────────────────┼────────────────────────────────────────────────────┤
│  Return address     │  x30 (LR) holds the return address at function     │
│  (x30)              │  entry                                             │
├─────────────────────┼────────────────────────────────────────────────────┤
│  Callee-saved       │  x19-x28, x29, x30, SP must be preserved across   │
│  registers          │  calls (but CAN have garbage at C entry point)     │
├─────────────────────┼────────────────────────────────────────────────────┤
│  Platform register  │  x18 is reserved for platform use (Linux: SCS)    │
│  (x18)              │  Should not be used by application code            │
└─────────────────────┴────────────────────────────────────────────────────┘
```

`init_cpu_task` establishes all of these invariants.

---

## 2. Register Classification in ARM64 ABI

```
ARM64 General-Purpose Registers — AAPCS64 Classification

 x0 - x7    : Argument / result registers (caller-saved)
 x8         : Indirect result location register (caller-saved)
 x9 - x15   : Temporary registers (caller-saved)
 x16 - x17  : Intra-procedure-call temporaries (IP0/IP1)
 x18        : Platform register (Linux: Shadow Call Stack)
 x19 - x28  : Callee-saved registers ← Linux boot path uses x19-x21
 x29        : Frame pointer (FP) — callee-saved
 x30        : Link register (LR) — callee-saved
 SP         : Stack pointer
 PC         : Program counter (not directly accessible)
 XZR/WZR    : Zero register (reads as 0, writes discarded)
```

### Boot Path Register Allocation (Linux-specific convention)

The Linux ARM64 boot path repurposes several callee-saved registers as
**boot-time global variables** that must survive from `primary_entry` to
`start_kernel`:

```
 x19 : MMU state flag (0 = was off, non-zero = was on at entry)
       Set by: record_mmu_state()
       Last used by: primary_entry (init_kernel_el branch)

 x20 : CPU boot mode (BOOT_CPU_MODE_EL1 or BOOT_CPU_MODE_EL2 | flags)
       Set by: init_kernel_el() return value
       Last used by: set_cpu_boot_mode_flag() + finalise_el2()
                     in __primary_switched

 x21 : FDT physical address
       Set by: preserve_boot_args() (copies x0 into x21)
       Last used by: str_l x21, __fdt_pointer in __primary_switched
```

These registers survive through:
- Multiple `bl` calls (callee-saved, so callee preserves them)
- `eret` in `init_kernel_el` (returns to `primary_entry`, callee-saved intact)
- Stack operations (callee-saved registers are saved/restored by callees)
- MMU enable (registers are CPU-local, MMU state change doesn't affect them)

`init_cpu_task` does NOT touch x19-x21, preserving the boot convention
until the values are consumed in `__primary_switched`.

---

## 3. Stack Alignment Analysis

AAPCS64 requires SP to be 16-byte aligned at all `bl`/`blr` call sites.

```asm
// init_cpu_task stack setup:
ldr    tmp1, [x4, #TSK_STACK]       // init_task.stack (16-byte aligned by design)
add    sp,   tmp1, #THREAD_SIZE      // + THREAD_SIZE (power of 2 → aligned)
sub    sp,   sp,   #PT_REGS_SIZE     // - 336 bytes
```

Is `PT_REGS_SIZE` a multiple of 16?

```c
// struct pt_regs layout (arch/arm64/include/asm/ptrace.h):
struct pt_regs {
    u64 regs[31];     // 248 bytes  (31 × 8)
    u64 sp;           //   8 bytes
    u64 pc;           //   8 bytes
    u64 pstate;       //   8 bytes
    u64 orig_x0;      //   8 bytes
    s32 syscallno;    //   4 bytes
    u32 unused2;      //   4 bytes
    u64 sdei_ttbr1;   //   8 bytes  (or pmr_save)
    u64 stackframe[2];// 16 bytes   (fp + lr)
    u64 frame_type;   //   8 bytes  (FRAME_META_TYPE_*)
                      //   8 bytes  padding
                      // ────────────────
                      // Total: 336 = 0x150 bytes
};

336 / 16 = 21.0 → 336 IS a multiple of 16. ✓
```

Therefore `sp = init_task.stack + THREAD_SIZE - 336` maintains 16-byte
alignment, satisfying AAPCS64 for all subsequent `bl` instructions.

---

## 4. Frame Record Chain — AAPCS64 Unwind Compliance

AAPCS64 defines the frame record format that all ARM64 debuggers and
stack unwinders rely on:

```
Standard AAPCS64 frame record (at x29):
  [x29 + 0]:  prev_fp  — points to caller's frame record (or 0 if bottom)
  [x29 + 8]:  prev_lr  — return address to caller (x30 at entry to function)
```

`init_cpu_task` creates the **terminal frame record** that anchors the chain:

```
init_cpu_task:
  stp  xzr, xzr, [sp, #S_STACKFRAME]   // {prev_fp=0, prev_lr=0}
  str  1,   [sp, #S_STACKFRAME_TYPE]   // type = FRAME_META_TYPE_FINAL
  add  x29, sp,  #S_STACKFRAME         // x29 = &this_frame_record

Result:
  x29 → ┌──────────────────────────────────┐
        │  prev_fp:  0                     │ ← signals "metadata frame"
        ├──────────────────────────────────┤
        │  prev_lr:  0                     │
        ├──────────────────────────────────┤
        │  type:     1 (FINAL)             │ ← signals "stop successfully"
        └──────────────────────────────────┘
```

After the `stp x29, x30, [sp, #-16]!` prologue in `__primary_switched`,
the chain becomes:

```
__primary_switched frame (pushed by stp x29, x30):
  [sp + 0]: prev_fp = old x29 → ┌──────────────────┐
  [sp + 8]: prev_lr = x30                          │
                                  │ FINAL frame (above)
                                  └──→ {0, 0, type=1}
```

`start_kernel` will be called with x29 set to `sp` (after `mov x29, sp`),
creating a proper frame chain from every deep panic call site all the way
back to this terminal record.

---

## 5. AAPCS64 Parameter Passing: What `start_kernel` Receives

`start_kernel` has the signature:
```c
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
```

No parameters, no return value. The `bl start_kernel` instruction transfers
control with:
- x0..x7: undefined (caller-saved, free)
- x29: valid frame pointer (established in __primary_switched prologue)
- x30: return address (set to `__primary_switched`'s `ASM_BUG()` address)
- SP: 16-byte aligned kernel stack

This is a fully AAPCS64-compliant call site.

---

## 6. The Per-CPU Offset and C ABI: How `this_cpu_ptr` Works at the ABI Level

The per-CPU infrastructure uses a GCC/Clang extension that generates
extremely efficient code:

```c
// Conceptual C expansion of this_cpu_read(cpu_number):
asm volatile("mrs %0, tpidr_el1" : "=r" (offset));
return *(typeof(cpu_number) *)((ulong)&cpu_number + offset);
```

But the actual kernel implementation goes further — it uses **segment-relative
addressing** analogous to x86 GS-relative memory references:

```c
#define raw_cpu_ptr(ptr) \
    (typeof(ptr))((unsigned long)(ptr) + __this_cpu_preempt_check("ptr"))

#define SHIFT_PERCPU_PTR(__p, __offset)    \
    RELOC_HIDE((typeof(*(__p)) __kernel __force *)(__p), (__offset))
```

The GCC/Clang backend, when targeting ARM64, compiles per-CPU accesses
into a single `ldr/str` with base register = `TPIDR_EL1` + variable_offset,
rather than computing the address explicitly. This is as optimal as
segment-relative addressing on x86.

### ABI Contract: x18 is Off-Limits for Compiler-Generated Code

When `CONFIG_SHADOW_CALL_STACK` is enabled, the compiler is informed (via
`-ffixed-x18`) that x18 is a **reserved register** — it will never use x18
for code generation. This means:
- No AAPCS64 parameter passing through x18
- No temporary value storage in x18
- All x18 usage is explicit SCS operations (push/pop via dedicated sequences)

Any violation of this constraint (e.g., linking against a library compiled
without `-ffixed-x18`) would corrupt the Shadow Call Stack, leading to
return address mismatches and silent incorrect execution or crashes.
