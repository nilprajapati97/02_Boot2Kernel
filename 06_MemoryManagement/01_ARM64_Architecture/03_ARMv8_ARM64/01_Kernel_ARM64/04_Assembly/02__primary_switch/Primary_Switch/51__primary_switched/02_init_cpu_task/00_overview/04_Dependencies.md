# `init_cpu_task` Dependencies — What Must Exist Before It Runs

## Preconditions — The "Contract" from the Caller

`init_cpu_task` can only run correctly if ALL of the following are true:

---

### Precondition 1: MMU ON and Virtual Addresses Valid

The macro uses `adr_l` to compute the virtual address of `__per_cpu_offset`. 
`adr_l` generates a PC-relative instruction. For this to give the correct virtual 
address, the MMU must be ON and the kernel must be running at its linked virtual address.

If MMU is OFF: `adr_l` gives the physical address, not virtual. 
`__per_cpu_offset` lives in virtual space. Writing the physical address into 
`tpidr_el1` would cause every `this_cpu_read()` to access the wrong memory.

---

### Precondition 2: `init_task` and `init_stack` Are Accessible

The macro reads `init_task.stack` (to get `init_stack` pointer) and
`init_task.thread_info.cpu` and `init_task.thread_info.scs_sp`.

These objects are in `.data` and `.bss` sections (BSS = zero-initialized). 
Before the macro runs:
- The kernel image must be mapped into virtual memory (done by MMU setup)
- `.bss` must be zeroed (done by `clear_bss_pt_regs` or by the bootloader)
- `init_task.stack` must point to valid `init_stack` memory

---

### Precondition 3: `__per_cpu_offset` Array Initialized

The array `__per_cpu_offset[NR_CPUS]` is in `.data` (or `.bss`). For the primary 
CPU, `__per_cpu_offset[0]` is set up by `setup_per_cpu_areas()` LATER in 
`start_kernel`. 

**But wait** — `init_cpu_task` runs BEFORE `start_kernel`! 

How does this work? For the primary CPU, `__per_cpu_offset[0]` is initialized to
the identity offset between the per-CPU prototype section and the CPU0 copy. Since
CPU0's per-CPU data IS the prototype, `__per_cpu_offset[0]` can be 0 or a small
fixed value known at link time, pre-initialized in the `.data` section.

Actually, `__per_cpu_offset[0]` is computed at kernel startup by 
`__cpu_setup` or initialized at build time to the per-CPU section base offset. 
The per-CPU section for CPU0 is the statically allocated `.data..percpu` section.

---

### Precondition 4: Cache State — D-Cache ON and Consistent

The macro performs memory reads (`ldr` from `init_task`) and memory writes 
(`stp` to `init_stack`). These require the D-cache to be ON and operating in 
write-back mode for performance.

At entry to `__primary_switched`, the MMU is ON which implies D-cache is ON 
(SCTLR_EL1.C = 1). Cache coherency is maintained because:
- `init_task` is in a physically-mapped normal-memory region
- `init_stack` is in the same region
- The MMU uses the same page tables set up during `__primary_switch`

---

### Precondition 5: x4 Holds `&init_task` (Caller's Responsibility)

The macro takes `tsk` as a parameter. The caller MUST load `&init_task` before
calling:
```asm
adr_l   x4, init_task      // MUST come before init_cpu_task
init_cpu_task x4, x5, x6
```

This precondition is a naming convention, not an architectural enforcement. 
If the caller passes a NULL or garbage pointer, the macro will silently corrupt
CPU state — there is no defensive check inside the macro. The kernel assumes this
contract is upheld.

---

### Precondition 6: PSTATE.DAIF — Exceptions Disabled

During `init_cpu_task`, exceptions should be disabled (or at minimum, the 
exception vectors should be valid). The VBAR_EL1 register is installed AFTER
`init_cpu_task` runs. If an exception occurs during `init_cpu_task`:

- VBAR_EL1 might point to garbage (from firmware)
- The exception handler cannot find `current` (sp_el0 might be mid-update)
- Catastrophic crash

The kernel ensures PSTATE.DAIF = 1 (all exceptions masked) throughout 
`__primary_switched` until after VBAR_EL1 is set.

---

## Post-conditions — What Is Guaranteed After

After `init_cpu_task` returns (macro completes):

1. `sp_el0 = &init_task` → `current` macro works
2. `sp = init_stack_top - PT_REGS_SIZE` → C stack is valid
3. `x29 = &init_stack_pt_regs.stackframe` → unwind chain terminates cleanly
4. `tpidr_el1 = __per_cpu_offset[0]` → `this_cpu_read()` works for CPU0
5. `x18 = init_task.thread_info.scs_sp` → SCS is active (if enabled)
6. `[sp + S_STACKFRAME] = {fp=0, lr=0}` → unwind sentinel in stack memory
7. `[sp + S_STACKFRAME_TYPE] = FINAL` → unwinder knows where to stop

---

## Formal Dependency Table

| Resource Needed | Provided By | When |
|---|---|---|
| Virtual address space | MMU setup in `__primary_switch` | Before `__primary_switched` entry |
| `init_task` mapped in VA | Kernel image mapping | Before `__primary_switched` entry |
| `init_stack` zeroed | Bootloader / `memset` in early init | Before entry |
| `__per_cpu_offset[0]` | Pre-initialized in `.data` | At compile/link time |
| Exceptions masked | PSTATE.DAIF=1 from `primary_entry` | Maintained throughout |
| Valid FDT pointer in x0 | Bootloader | Before entry, preserved across macro |

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