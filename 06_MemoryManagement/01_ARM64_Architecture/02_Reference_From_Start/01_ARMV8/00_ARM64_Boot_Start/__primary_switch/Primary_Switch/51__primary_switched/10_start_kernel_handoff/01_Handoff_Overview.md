# `bl start_kernel` — The Point of No Return

## The Final Instruction in `__primary_switched`

```asm
// arch/arm64/kernel/head.S __primary_switched (final lines):
    ...
    mov     x0, x20              // boot mode (passed to finalise_el2)
    bl      finalise_el2         // EL2 finalization
    
    bl      start_kernel         // ← THE HANDOFF — no return expected
    
    // Should never reach here:
ASM_BUG()                        // triggers BUG() if start_kernel returns
SYM_FUNC_END(__primary_switched)
```

`start_kernel` is the first C function in the init path that:
1. Never returns (in normal operation)
2. Receives no arguments (all state is in global variables and registers)
3. Runs the entire Linux kernel initialization sequence

---

## What `start_kernel` Does (Condensed)

```c
// init/main.c
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
{
    char *command_line;
    char *after_dashes;
    
    // 1. Architecture-specific setup:
    set_task_stack_end_magic(&init_task);  // stack overflow detection
    smp_setup_processor_id();
    debug_objects_early_init();
    
    // 2. Kernel core init:
    cgroup_init_early();
    local_irq_disable();              // interrupts disabled for now
    early_boot_irqs_disabled = true;
    
    // 3. Architecture init:
    boot_cpu_init();
    page_address_init();
    pr_notice("%s", linux_banner);    // "Linux version x.x.x ..."
    
    // 4. Memory management:
    setup_arch(&command_line);        // ARM64: ACPI/DTB, NUMA, memory map
    mm_init();                        // slab, vmalloc, etc.
    
    // 5. Scheduler and timers:
    sched_init();
    rcu_init();
    init_timers();
    hrtimers_init();
    
    // 6. Console and I/O:
    console_init();
    
    // 7. Everything else:
    vfs_caches_init();
    signals_init();
    
    // 8. Enter user space:
    arch_call_rest_init();   // → rest_init() → init process PID 1
    // ← Never returns
    
    prevent_tail_call_optimization();  // GCC annotation
}
```

---

## Register State When `bl start_kernel` Executes

At the moment of `bl start_kernel`:
```
Register  Value             Source
────────────────────────────────────────────────────────────────
x0        0                 (no argument needed)
x1-x7     undefined         (start_kernel takes no args)
x19       SPSR from EL3     (saved in __primary_switch)
x20       boot mode         (EL1=0 or EL2=non-zero)
x21       FDT physical addr (saved from x0 at entry)
x22       kimage PA         (passed from __primary_switch)
x29       sp - 16           (C stack frame base = x29)
x30       return address    ← set by bl instruction
sp        init_stack end    (aligned stack pointer)
TTBR0/1   kernel page tables (set up during __primary_switch)
SCTLR_EL1 MMU on, caches on (set up during __primary_switch)
VBAR_EL1  vectors           (set up in __primary_switched)
TPIDR_EL1 per-cpu offset    (set up in init_cpu_task)
```

`start_kernel` calls C code — it uses `x0`-`x7` for arguments and `x19`-`x28`
for callee-saved context. All the state from `__primary_switched` has been
saved to global variables (not registers) by this point.

---

## `bl` vs `b` — Why `bl` Is Used

```asm
bl  start_kernel    // Branch with Link
// vs
b   start_kernel    // Branch without Link
```

`bl` saves the return address in `x30`:
- `x30 = address of next instruction after bl`
- If `start_kernel` ever returns, it would return to `ASM_BUG()` macro

This is intentional: the `ASM_BUG()` after `bl start_kernel` is a safety net:
```c
#define ASM_BUG()   BUG_INSTR  // generates ARM64 BRK instruction
                                 // causes kernel panic with stack trace
```

If `start_kernel` somehow returned (major kernel bug), `ASM_BUG()` ensures
a controlled crash with diagnostic output rather than undefined behavior.

Using `b` instead would work but would lose the safety net (no return address
for stack trace). Using `bl` is the defensive programming choice.

---

## The "Never Returns" Contract

`start_kernel` never returns because it ends with `arch_call_rest_init()`:
```c
// init/main.c:
static noinline void __ref rest_init(void)
{
    ...
    kernel_thread(kernel_init, NULL, CLONE_FS);     // PID 1
    kernel_thread(kthreadd, NULL, CLONE_FS|CLONE_FILES); // PID 2
    ...
    schedule_preempt_disabled();   // yield to init process
    
    // Init task becomes idle task here:
    cpu_startup_entry(CPUHP_ONLINE);  // → idle loop (never returns)
}
```

The init task (`PID 0`, the task set up by `init_cpu_task` in `__primary_switched`)
becomes the **idle task**. The idle task runs `cpu_idle_loop()` when no other
task is runnable — it never terminates.

So the chain is:
```
__primary_switched
    → bl start_kernel
        → arch_call_rest_init
            → rest_init
                → create PID 1 (kernel_init → /sbin/init)
                → create PID 2 (kthreadd → kernel threads)
                → cpu_startup_entry  ← idle loop for CPU0 FOREVER
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The handoff from __primary_switched to start_kernel is the transition from assembly to C. At this point the CPU state is:
- EL1 (or EL2 if VHE), AArch64 execution state.
- MMU on (SCTLR_EL1.M=1), I-cache on (I=1), D-cache on (C=1).
- SP_EL1 = init_thread_union + THREAD_SIZE (valid kernel stack).
- SP_EL0 = &init_task (current task pointer).
- TTBR0_EL1 = identity map root, TTBR1_EL1 = kernel page table root.
- VBAR_EL1 = vectors (exception handler table).
- TPIDR_EL1 = per-CPU base for boot CPU.
This is the minimum viable CPU state for the C kernel.

### Kernel Perspective (Linux ARM64)
start_kernel() is the first C function called after __primary_switched. It calls:
  setup_arch() -> which calls:
    paging_init()     // rebuild final page tables
    bootmem_init()    // initialize memory allocator
    unflatten_device_tree() // parse FDT
  trap_init()         // set up exception handlers
  mm_init()           // memory management init
  sched_init()        // scheduler
  rest_init()         // start init process (PID 1)
The register state established in __primary_switched (VBAR, TPIDR, SP_EL0, etc.) remains valid until start_kernel reinitializes each subsystem.

### Memory Perspective (ARMv8 Memory Model)
At the start_kernel handoff, the ARMv8 memory model is fully active: the CPU sees Normal Inner-Shareable Write-Back Cacheable DRAM for all kernel code and data. The linear map (PAGE_OFFSET + PA) provides access to all physical RAM. The early page tables (init_pg_dir, __idmap_pg_dir) are still in use -- paging_init() will replace them with the definitive swapper_pg_dir. Until paging_init() runs, the identity map (TTBR0_EL1) remains valid; after paging_init(), TTBR0_EL1 is cleared (TCR_EL1.EPD0=1) to prevent accidental user-VA accesses from the kernel.