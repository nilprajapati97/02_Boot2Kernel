# Register Usage Inside `init_cpu_task`

## Register Roles — Before, During, After

```
Register  | Role in Macro        | Value Before     | Value After
──────────────────────────────────────────────────────────────────────
x4 (tsk)  | task_struct pointer  | &init_task       | &init_task (unchanged)
x5 (tmp1) | scratch/result       | don't care       | __per_cpu_offset[0]
x6 (tmp2) | CPU ID scratch       | don't care       | 0 (CPU0 ID)
x18       | SCS pointer          | don't care       | scs_sp (if SCS enabled)
x29       | frame pointer (fp)   | 0                | &pt_regs.stackframe in init_stack
sp        | stack pointer        | early_init_stack | init_stack top - 336
sp_el0    | current task (EL1)   | undefined        | &init_task
tpidr_el1 | per-CPU base (sysreg)| undefined        | __per_cpu_offset[0]
```

---

## Data Flow Graph — Register Dependencies

```
x4 (&init_task)
 │
 ├──────────────────────► msr sp_el0, x4   →  sp_el0 = &init_task
 │                                              │
 │                                              └── scs_load_current uses sp_el0
 │                                                  mrs x18, sp_el0
 │                                                  ldr x18, [x18, #SCS_SP_OFF]
 │
 ├── ldr x5, [x4, #TSK_STACK]   →   x5 = init_stack base
 │    │
 │    └── add sp, x5, #THREAD_SIZE
 │          sub sp, sp, #PT_REGS_SIZE
 │                │
 │                └── stp xzr, xzr, [sp, #S_STACKFRAME]
 │                    str x5, [sp, #S_STACKFRAME_TYPE]
 │                    add x29, sp, #S_STACKFRAME
 │
 └── ldr w6, [x4, #TSK_TI_CPU]   →   w6 = 0 (CPU ID)
      │
      └── adr_l x5, __per_cpu_offset     →  x5 = &__per_cpu_offset[0]
           ldr x5, [x5, x6, lsl #3]     →  x5 = __per_cpu_offset[0]
           msr tpidr_el1, x5
```

---

## Why `x4`, `x5`, `x6` Specifically?

The calling convention at `__primary_switched` entry point:
- `x0` = physical address of FDT (passed from bootloader)
- `x1`–`x3` = undefined/clobbered during early MMU setup

Registers x4-x28 are "caller-saved" scratch at this point. The choice of x4/x5/x6
is arbitrary but consistent — the kernel uses these registers throughout the early
boot assembly.

After `init_cpu_task` completes, the caller (`__primary_switched`) continues to
use x0 (FDT pointer) for subsequent operations. The macro parameters were chosen
to NOT conflict with x0-x3 which carry boot parameters.

---

## System Register Reads and Writes

The macro performs these system register accesses:

### Writes:
1. `msr sp_el0, x4` — EL0 SP, repurposed as current task
2. `msr tpidr_el1, x5` — Thread ID register EL1, per-CPU offset

### Reads:
1. `mrs x18, sp_el0` — Read back just-set sp_el0 (in `scs_load_current`)

### Implicit (via SP manipulation):
3. `sp` is written 3 times (add, sub after ldr) — implicit MSR to the banked SP_EL1

---

## `w6` vs `x6` — 32-bit Read for CPU ID

```asm
ldr     w6, [x4, #TSK_TI_CPU]    // 32-bit load: thread_info.cpu is u32
ldr     x5, [x5, x6, lsl #3]    // x6 used in address calc (zero-extended)
```

`thread_info.cpu` is `u32` (32-bit). Using `w6` (32-bit register) performs a
32-bit load. On ARM64, loading into a `w` register **zero-extends** to the full
64-bit `x` register automatically. So `x6 = (u64)thread_info.cpu`.

Then `x6, lsl #3` computes `cpu * 8` as the byte offset into the 64-bit array
`__per_cpu_offset`. This is correct for any CPU ID (0 through NR_CPUS-1).

---

## x29 After the Macro — The Boot Frame Pointer Chain

```
Stack memory layout after init_cpu_task:

VA: init_stack_base + THREAD_SIZE - PT_REGS_SIZE
    │
    │  [sp + 0]:             x0 save area (for pt_regs.regs[0])
    │  ...
    │  [sp + S_STACKFRAME]:  stackframe.fp  = 0   ← x29 points HERE
    │  [sp + S_STACKFRAME + 8]: stackframe.pc = 0
    │  [sp + S_STACKFRAME_TYPE]: type = FRAME_META_TYPE_FINAL
    │
VA: init_stack_base + THREAD_SIZE  (stack grows DOWN from here)

x29 = sp + S_STACKFRAME = VA of the stackframe record
```

This means: stack traces start at `x29`, follow `fp` chains upward, and terminate
when they reach `x29 = init_stack_top.pt_regs.stackframe` where `fp = 0`.

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