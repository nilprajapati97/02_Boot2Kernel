# `start_kernel` — Caller/Callee Contract

## ABI at the Assembly/C Boundary

ARM64 uses AAPCS64 (ARM Architecture Procedure Call Standard for AArch64).
At the `bl start_kernel` call site:

```
AAPCS64 Contract:
─────────────────────────────────────────────────────
CALLER responsibility (assembly in __primary_switched):
    • x0-x7:   argument registers (start_kernel takes none, so undefined)
    • x8:      indirect result register (not used)
    • x29:     set to current frame (done by stp x29, x30 earlier)
    • x30:     set by bl instruction (return address = ASM_BUG() location)
    • SP:      16-byte aligned (ensured by init_stack alignment)
    
CALLEE responsibility (start_kernel C code):
    • x19-x28: MUST be preserved (callee-saved)
    • x29:     MUST be preserved (frame pointer)
    • x30:     MUST be preserved (link register)
    • x0-x7:   MAY be clobbered (caller-saved)
    • x8-x18:  MAY be clobbered (caller-saved)
    • SP:      MUST be restored on return
```

For `start_kernel` specifically, the CALLEE contract doesn't matter much
because `start_kernel` never returns. But GCC generates code as if it will.

---

## Stack Frame Chain at `start_kernel` Entry

```
Initial stack frame (set up in __primary_switched):

Address            Content
───────────────────────────────────────────────────
init_stack_top    ← initial SP value set in init_cpu_task
     -8           │ x30 (original LR = 0, "no caller" marker)
     -16          │ x29 (frame pointer = 0, "no frame" marker)
       ← SP ─────┘  (SP = init_stack_top - 16 after stp x29,x30,[sp,-16]!)

When start_kernel executes its own function prologue:
     -16 from SP  │ x30 (saved LR = return to ASM_BUG() in __primary_switched)
     -24 from SP  │ x29 (saved x29 = previous frame ptr)
       ← new SP ─┘

Stack unwind would show:
    start_kernel (no meaningful locals yet)
        ← called from __primary_switched (x29 chain)
            ← bottom of stack (x29 = 0, x30 = 0)
```

The `final_frame_marker` (setting x29=0, x30=0 in `init_cpu_task`) ensures
stack unwinders stop cleanly at the bottom.

---

## `__no_sanitize_address` on `start_kernel`

```c
// init/main.c:
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
```

Wait — `start_kernel` itself is marked `__no_sanitize_address`!

Why? Because `start_kernel` runs before `kasan_init()` (the FULL KASAN
initialization). Although `kasan_early_init` set up minimal shadow mapping
for the kernel image, the early parts of `start_kernel` (before `mm_init`)
run with incomplete KASAN shadow. Marking `start_kernel` as no-sanitize
prevents instrumentation in the function itself.

Functions called by `start_kernel` after `mm_init` and `kasan_init` CAN be
KASAN-instrumented normally.

---

## `asmlinkage` on `start_kernel`

```c
asmlinkage void start_kernel(void)
// vs
void start_kernel(void)
```

`asmlinkage` is a GCC attribute that tells the compiler to pass ALL arguments
via the stack (not registers). On ARM64, this is rarely needed (AAPCS64 uses
registers for args), but `asmlinkage` is kept for historical compatibility
and for x86 where it matters.

On ARM64, `asmlinkage void func(void)` is effectively the same as `void func(void)`.
But using it is important: if someone accidentally adds a parameter to `start_kernel`,
the `asmlinkage` attribute would cause it to be stack-passed, which would catch
the mistake quickly (assembly caller hasn't pushed any stack args).

---

## SMP Synchronization at `start_kernel`

By the time `bl start_kernel` executes (primary CPU), secondary CPUs are in
one of these states:
```
Secondary CPU state at primary's start_kernel entry:
    1. Not yet started (spinning at WFE / pen-holding release)
    2. Waiting at secondary_holding_pen (polling for release word)
    3. Executing secondary_start_kernel (if SMP was started before start_kernel)

Typical ARM64 PSCI boot:
    • All secondary CPUs are powered OFF at this point
    • Primary calls smp_init() inside start_kernel
    • smp_init → cpu_up → PSCI_CPU_ON for each secondary CPU
    • Secondary CPUs then execute secondary_entry → secondary_startup
    → secondary_start_kernel (the C equivalent of start_kernel for secondaries)
```

Secondary CPUs do NOT call `start_kernel`. They call `secondary_start_kernel`:
```c
// arch/arm64/kernel/smp.c
asmlinkage notrace void secondary_start_kernel(void)
{
    // Much shorter than start_kernel!
    // CPU-local init only: caches, timers, etc.
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);  // idle loop
}
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The AAPCS64 (ARM Architecture Procedure Call Standard for AArch64) defines the calling convention for ARMv8-A:
- x0-x7: argument/result registers (caller-saved).
- x8: indirect result location (caller-saved).
- x9-x15: caller-saved temporaries.
- x16-x17: intra-procedure-call scratch (IP0/IP1).
- x18: platform register (shadow call stack pointer on Linux).
- x19-x28: callee-saved registers (must be preserved by the callee).
- x29 (FP): frame pointer (callee-saved).
- x30 (LR): link register (return address, caller-saved in the sense that BL writes it).
- SP: stack pointer (must be 16-byte aligned at any public function entry).
The CPU enforces 16-byte SP alignment via SCTLR_EL1.SA when enabled.

### Kernel Perspective (Linux ARM64)
At the boundary between assembly (head.S) and C code (start_kernel), the kernel must comply with AAPCS64: SP must be 16-byte aligned, x29 must be 0 (null frame pointer to terminate the unwind chain), and the first argument (x0) must hold the FDT pointer. The kernel assembly sets these up explicitly before the BL start_kernel. The frame pointer chain (x29 -> previous x29 -> ... -> 0) is the basis for the kernel stack unwinder (arch/arm64/kernel/stacktrace.c).

### Memory Perspective (ARMv8 Memory Model)
The ABI boundary involves stack memory: the callee's frame is at [SP - frame_size, SP). Each frame contains saved x29 (previous FP) and x30 (return address) as a pair at [SP, SP+16) -- the standard frame record. The stack grows downward in memory. Because the stack is Normal Inner-Shareable Write-Back Cacheable memory, frame save/restore operations (STP x29, x30, [sp, #-16]!) benefit from L1 D-cache hits during recursive function calls that stay within a cache set. The 16-byte alignment requirement ensures STP/LDP instructions are naturally aligned, avoiding unaligned access penalties or faults.