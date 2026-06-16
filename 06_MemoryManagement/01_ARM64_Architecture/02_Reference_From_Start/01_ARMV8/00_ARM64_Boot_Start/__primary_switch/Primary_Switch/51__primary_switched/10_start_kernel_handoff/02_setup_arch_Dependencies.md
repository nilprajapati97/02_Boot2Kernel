# `setup_arch` — Architecture Layer Initialization After Handoff

## `setup_arch` on ARM64

The first major architecture-specific function called by `start_kernel`:
```c
// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    /*
     * 1. Initialize the FDT (Device Tree Blob) that was saved in __primary_switched:
     *    __fdt_pointer was set by: str_l x21, __fdt_pointer
     */
    setup_machine_fdt(__fdt_pointer);    // ← Uses the saved FDT pointer!
    
    /*
     * 2. Parse the command line from FDT:
     */
    *cmdline_p = boot_command_line;
    
    /*
     * 3. Build the early memory map (memblock):
     *    Uses __fdt_pointer to find memory nodes in FDT
     */
    parse_early_param();
    early_fixmap_init();
    early_ioremap_init();
    
    /*
     * 4. CPU feature detection:
     *    Reads ID_AA64MMFR0_EL1, ID_AA64PFR0_EL1, etc.
     */
    setup_machine_fdt(__fdt_pointer);
    
    /*
     * 5. Paging setup (full page tables):
     *    kimage_voffset (set in __primary_switched) is used here
     */
    paging_init();               // ← Uses kimage_voffset!
    
    /*
     * 6. Everything else:
     *    NUMA, SMP, ACPI if present, etc.
     */
}
```

Notice: `setup_arch` depends on BOTH `__fdt_pointer` AND `kimage_voffset` —
both set up in `__primary_switched`. This confirms why those operations had to
happen in `__primary_switched`.

---

## Init Task Lifetime: From `init_cpu_task` to `cpu_idle_loop`

The `init_task` (PID 0) has a lifecycle:

```
Phase 1: Assembly (in __primary_switched)
    init_cpu_task sets:
        SP_EL0 = &init_task         ← current task
        SP (SP_EL1) = stack top     ← kernel stack
        TPIDR_EL1 = 0               ← CPU 0 per-cpu offset
    
Phase 2: C initialization (in start_kernel)
    init_task is used as:
        - Current task for CPU 0
        - Context for running start_kernel()
        - Context for running rest_init()
    
Phase 3: Scheduler started (in sched_init + first schedule())
    scheduler takes over, init_task becomes "swapper/0"
    
Phase 4: rest_init() fork:
    kernel_thread(kernel_init, ...)  → PID 1 ("init")
    kernel_thread(kthreadd, ...)     → PID 2 ("kthreadd")
    
Phase 5: Idle task (forever)
    cpu_startup_entry(CPUHP_ONLINE)
        → cpu_idle_loop()
            → while (1) { do_idle(); }  ← ARM64 WFI instruction
```

The `init_task` structure that `init_cpu_task` pointed to in `__primary_switched`
is the same `init_task` that runs `start_kernel` and eventually becomes idle.

---

## Memory Map at `start_kernel` Entry

```
Virtual Address Space (at start_kernel time):

0xffff_ffff_ffff_ffff ┐
0xffff_ff80_0000_0000  │ vmalloc region (not yet set up)
0xffff_fc00_0000_0000 ─┤ KASAN shadow (if enabled, mapped by kasan_early_init)
0xffff_e000_0000_0000  │
0xffff_c000_0000_0000 ─┤ Linear map start (RAM direct-mapped)
0xffff_8000_10xx_xxxx ─┤ Kernel .text (KASLR base + 16M alignment)
0xffff_8000_1000_0000  │   .text   (code)
                       │   .rodata (read-only data)
                       │   .data   (initialized data)
                       │   .bss    (zero-initialized data)
0xffff_8000_xxxx_xxxx ─┤ Kernel .bss end (_end symbol)
                       │
                       │ init_stack (4 pages = 16KB, at _end + offset)
                       │   [top] ← SP_EL1 = init task stack
                       │   x29 ← frame pointer (0, bottom frame)
                       │   x30 ← return address for start_kernel's caller
```

After `paging_init()` runs (inside `setup_arch`), the full linear map is set up
covering ALL physical RAM.

---

## Argument Convention: Why `start_kernel(void)`

`start_kernel` takes NO arguments. This is intentional:
- All boot-time parameters are saved to GLOBAL variables in `__primary_switched`
- `__fdt_pointer`: FDT saved here
- `kimage_voffset`: VA-PA offset saved here
- `__boot_cpu_mode`: boot mode saved here
- `kaslr_offset`: KASLR random seed saved by earlier code

This design means:
1. `start_kernel` has a stable ABI (no register arguments to get wrong)
2. Secondary CPUs that branch to their C entry point don't need argument passing
3. Crash debuggers can always find these values (they're at known symbols)
4. `kexec` can inspect these values to understand the running kernel

The alternative (passing everything as arguments) would be fragile:
ARM64 ABI passes arguments in x0-x7, and by the time `bl start_kernel` executes,
some of those may have been clobbered by intermediate function calls.

---

## What `start_kernel` Cannot Undo

Once `start_kernel` begins, several configurations are PERMANENT:
1. **MMU/cache state**: Cannot turn off MMU once C code runs
2. **VBAR_EL1**: Exception vectors; changing them would break running C code
3. **init_task**: PID 0 is the running task; cannot be replaced
4. **TPIDR_EL1**: Per-cpu offset; used by every `this_cpu_*` operation
5. **kimage_voffset**: The KASLR offset; used for all VA↔PA conversions

The `__primary_switched` function made all these commitments on behalf of the
kernel. There is no "undo" from the moment `bl start_kernel` executes.

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