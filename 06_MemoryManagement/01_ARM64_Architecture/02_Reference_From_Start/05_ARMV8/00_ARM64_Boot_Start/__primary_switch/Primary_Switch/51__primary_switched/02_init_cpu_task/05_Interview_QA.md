# `init_cpu_task` — Interview Q&A Deep Dive

---

## Q1: What does `msr sp_el0, tsk` accomplish and why use `sp_el0`?

**A:** `sp_el0` is the banked EL0 stack pointer register. When executing at EL1,
`sp_el0` has no architectural role — the CPU doesn't use it for its own stack
operations. Linux repurposes it as a "current task register" holding `&init_task`.

The payoff is the `current` macro:
```c
static __always_inline struct task_struct *get_current(void) {
    unsigned long sp;
    asm("mrs %0, sp_el0" : "=r"(sp));
    return (struct task_struct *)sp;
}
```
One `mrs` instruction reads `current` in ~1 cycle with **zero memory traffic**.
Compare to fetching from a global pointer which requires a load from memory.

---

## Q2: Why does the stack switch reserve `PT_REGS_SIZE` bytes below the top?

**A:** The `PT_REGS_SIZE` (336 bytes) reservation at the stack top creates a
synthetic `struct pt_regs` that represents the "interrupted context" at the base
of the boot stack. This reservation serves two purposes:

1. **Unwind anchor:** The frame sentinel written into `[sp, #S_STACKFRAME]` creates
   the final entry in the unwind chain. Without this, stack traces from `start_kernel`
   would crash the unwinder.

2. **KPTI conformance:** On KPTI-enabled kernels, the trampoline that switches from
   user to kernel space expects `pt_regs` at a known offset from the stack pointer.
   Having `pt_regs` reserved at the top of every kernel stack maintains this invariant
   even for the boot stack.

---

## Q3: Explain what `stp xzr, xzr, [sp, #S_STACKFRAME]` does and why `xzr`.

**A:** This instruction stores two 64-bit zeros into the `stackframe` field of the
`pt_regs` at the top of `init_stack`. The `stackframe` field contains:
```c
struct stackframe_record {
    u64 fp;   // [sp + S_STACKFRAME + 0] = 0
    u64 pc;   // [sp + S_STACKFRAME + 8] = 0
};
```
Setting `fp = 0` is the **unwind sentinel**: the ARM64 stack unwinder in
`arch/arm64/kernel/stacktrace.c` terminates its walk when it encounters `fp = 0`.
Setting `pc = 0` avoids printing a garbage "called from 0x0" in stack traces.

`xzr` (the zero register) is used because it is hardwired to 0 on ARM64 — no
need to `mov x5, #0` first; using `xzr` in a `stp` is a single-instruction zero-fill.

---

## Q4: On a production Android Pixel kernel, is SCS always enabled?

**A:** Yes, on Google Pixel devices (ARM64) since approximately Pixel 3 / Android 10.
SCS (`CONFIG_SHADOW_CALL_STACK`) is enabled in their kernel configs. The `x18`
register is reserved by Google's kernel ABI as the SCS pointer and is NOT used by
GCC/Clang for general purpose register allocation in kernel code.

Key points for interview:
- `x18` is caller-saved in the standard AAPCS64. Linux REDEFINES this: in the kernel,
  `x18` is a **reserved register** that must not be clobbered by any kernel code.
- Clang enforces this with `-ffixed-x18`.
- `scs_load_current` loads `init_task.thread_info.scs_sp` into `x18`.
- Each function call `BL` does NOT push to `x18` — instead, compiler-inserted SCS
  instrumentation does `str x30, [x18], #8` (push return address to shadow stack).
- On return, `ldr x30, [x18, #-8]!` pops and verifies.

---

## Q5: What is `__per_cpu_offset` and why does `tpidr_el1` point into it?

**A:** `__per_cpu_offset` is a `u64[NR_CPUS]` array. `__per_cpu_offset[cpu]` is the
offset (in bytes) from the per-CPU data section's **base copy** (`.data..percpu`)
to the **per-CPU copy** allocated for CPU `cpu`.

**How `this_cpu_read()` works:**
```c
// SIMPLIFIED:
#define this_cpu_read(var) \
    ({ \
        unsigned long __base; \
        asm("mrs %0, tpidr_el1" : "=r"(__base)); \
        *((typeof(var) *)(__base + __per_cpu_offset_of(var))); \
    })
```

Actually more directly — per-CPU symbols are at known **offsets** from the per-CPU
base. With `tpidr_el1` = `__per_cpu_offset[cpu]`, the per-CPU variable at section
offset `N` is at virtual address `__per_cpu_offset[cpu] + N`. The linker places
per-CPU variables at their offsets. `tpidr_el1` + linker offset = VA of the var.

**Memory layout:**
```
VA              Content
──────────────────────────────────────────
0xFFFF000010000000   per-CPU section BASE (prototype copy, CPU0)
0xFFFF000010010000   per-CPU section COPY for CPU0 (offset in tpidr_el1 for CPU0)
0xFFFF000010020000   per-CPU section COPY for CPU1
...
```
`__per_cpu_offset[0]` = difference between CPU0's copy VA and the prototype VA.

---

## Q6: What would happen if init_cpu_task was skipped entirely?

**A:** In order of which failure occurs first:

1. **`current` returns garbage** — first kernel C code that calls `current` (very
   early in `start_kernel`) will dereference a garbage pointer → translation fault
   or silent data corruption.

2. **Stack overflow/wrong stack** — `start_kernel` and its callees would be using
   `early_init_stack` (a temporary boot stack). When `early_init_stack` gets freed
   or reused, all kernel state on it gets corrupted.

3. **`this_cpu_read` crashes** — `tpidr_el1` = undefined. Any per-CPU access
   (including `smp_processor_id()`, `local_irq_disable()`, etc.) dereferences a
   garbage address.

4. **Stack unwinder crashes** — any `WARN_ON`, `BUG()`, panic, or Oops handling
   that tries to unwind the stack will loop infinitely or page-fault without the
   `fp=0` sentinel.

5. **No SCS protection** — `x18` = garbage, first function return may check shadow
   stack → immediate KFENCE/KASAN/SCS trap.

---

## Q7: Describe the sequence from `adr_l x4, init_task` to the first C instruction.

**A:**
```asm
// 1. Load init_task address (PC-relative, virtual address)
adr_l   x4, init_task

// 2. init_cpu_task macro BEGIN
//    OP1: set sp_el0 = &init_task
msr     sp_el0, x4

//    OP2: switch stack
ldr     x5, [x4, #TSK_STACK]          // x5 = init_stack base
add     sp, x5, #THREAD_SIZE           // sp = init_stack + 16384
sub     sp, sp, #PT_REGS_SIZE          // sp = init_stack + 16384 - 336

//    OP3: frame sentinel
stp     xzr, xzr, [sp, #S_STACKFRAME]
mov     x5, #FRAME_META_TYPE_FINAL
str     x5, [sp, #S_STACKFRAME_TYPE]
add     x29, sp, #S_STACKFRAME

//    OP4: SCS (if enabled)
mrs     x18, sp_el0
ldr     x18, [x18, #TSK_TI_SCS_SP]

//    OP5: per-CPU offset
adr_l   x5, __per_cpu_offset
ldr     w6, [x4, #TSK_TI_CPU]
ldr     x5, [x5, x6, lsl #3]
msr     tpidr_el1, x5
// init_cpu_task macro END

// 3. VBAR_EL1 install (exception vectors) [NEXT STEP AFTER init_cpu_task]
adr_l   x5, vectors
msr     vbar_el1, x5
isb

// 4. C frame setup (frame pointer established by init_cpu_task OP3 above)
// 5. bl start_kernel  ← FIRST C CALL
```

The total CPU state transition from chaos to "ready for C runtime" is approximately
15 assembly instructions. This is the minimum necessary — every instruction is load-bearing.

---

## Q8: How does `init_cpu_task` differ between primary and secondary CPU boot?

**A:** It does NOT differ — the **same macro** runs on every CPU. But the ARGUMENTS differ:

| Argument | Primary CPU | Secondary CPU |
|---|---|---|
| `tsk` (x4) | `&init_task` (adr_l) | `current_task` pointer from PSCI args |
| `tmp1` | x5 (scratch) | x5 (scratch) |
| `tmp2` | x6 (scratch) | x6 (scratch) |

For secondary CPUs, `secondary_start_kernel()` already received the `task_struct`
pointer in a register (set by `__cpu_up` → PSCI). Each secondary CPU gets its own
`task_struct` (the idle task for that CPU), its own stack (allocated by
`copy_process`), and its own per-CPU offset.

The common macro handles all CPUs identically — the per-CPU behavior comes from the
different `task_struct` and `__per_cpu_offset[cpu]` values for each CPU.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the current task (process) is identified at EL0 via TPIDR_EL0 (user thread ID) and at EL1 via SP_EL0. Linux uses SP_EL0 to store the pointer to the current task_struct. SP_EL0 is a dedicated register (not the EL0 stack pointer when running in EL1 -- at EL1, SP_ELx selects either SP_EL0 or SP_EL1 as the active stack, controlled by PSTATE.SP). When the kernel uses SP_EL0 to store the current task pointer, it is using SP_EL0 as a general-purpose register (reading/writing it with MRS/MSR SP_EL0).

### Kernel Perspective (Linux ARM64)
init_cpu_task is a per-CPU variable (or boot-time initialization) that sets up the idle task (init_task / swapper) as the current task. In __primary_switched:
  msr  sp_el0, x23        // x23 holds init_task VA, set SP_EL0 = &init_task
  ldr  x8, [x23, #TSK_TI_CPU]  // verify .cpu field
The current macro in Linux ARM64 expands to:
  mrs x0, sp_el0          // read SP_EL0 as current task_struct pointer
SP_EL0 is never spilled to the stack (it is a system register), making current() essentially a zero-cost operation.

### Memory Perspective (ARMv8 Memory Model)
task_struct for init_task lives in the .data section of the kernel image (statically allocated). Its VA is in the kernel text/data mapping (TTBR1_EL1). When SP_EL0 is set to &init_task, the memory region is already mapped and accessible. The task's stack (thread_union) is in the .init.data section and is also already mapped. After start_kernel -> sched_init(), all subsequent tasks have their task_struct allocated from slab memory in the kernel heap (also in the TTBR1_EL1 region).