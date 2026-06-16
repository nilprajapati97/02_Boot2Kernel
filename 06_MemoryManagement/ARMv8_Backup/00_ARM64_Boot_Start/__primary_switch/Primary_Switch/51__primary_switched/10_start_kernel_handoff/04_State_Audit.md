# Complete State Audit — What `__primary_switched` Leaves for `start_kernel`

## Full State Inventory

Everything `__primary_switched` configures before handing off:

### Hardware Registers
```
Register         Value Set By              Used By
───────────────────────────────────────────────────────────────────────────────
SP_EL1           init_stack top            start_kernel's C stack
SP_EL0           &init_task                current_task_ptr (this_cpu macros)
TPIDR_EL1        0 (CPU 0 per-cpu offset)  per_cpu() access, this_cpu_read/write
VBAR_EL1         vectors symbol            Exception handling (interrupts, syscalls)
SCTLR_EL1        MMU on, caches on         All virtual memory operations
TTBR0_EL1        idmap_pg_dir              Identity map (needed for early PA accesses)
TTBR1_EL1        swapper_pg_dir            Kernel virtual address space
MAIR_EL1         memory types configured   Cache policy for all mappings
TCR_EL1          48-bit VA configured      Translation control
HCR_EL2          (if EL2 boot) E2H/TGE    Hypervisor config (VHE or nVHE)
SPSR_EL2         (if EL2 boot) EL2h/EL1h  Mode after finalise_el2
```

### Global Variables
```
Variable             Set In               Value
────────────────────────────────────────────────────────────────────────────
__fdt_pointer        str_l x21, ...       Physical address of FDT blob
kimage_voffset       str_l x4, ...        VA - PA offset (KASLR)
__boot_cpu_mode[0]   set_cpu_boot_mode_   BOOT_CPU_MODE_EL1 or EL2
init_task.thread.cpu_context  init_cpu_task  CPU 0 task context
```

### Memory Layout
```
Region               State
────────────────────────────────────────────────────────────────────────────
Kernel .text         Mapped RX (read-execute), VA = PA + kimage_voffset
Kernel .rodata       Mapped R (read-only), VA = PA + kimage_voffset
Kernel .data/.bss    Mapped RW (read-write), VA = PA + kimage_voffset
Linear map (RAM)     NOT YET MAPPED (setup_arch/paging_init does this)
vmalloc              NOT YET MAPPED
KASAN shadow         Minimal mapping (zero page) if CONFIG_KASAN
```

---

## Dependency Graph

```
                    ┌─────────────────────────────────────┐
                    │        start_kernel()                │
                    └────────────┬────────────────────────┘
                                 │ requires:
        ┌────────────┬───────────┼───────────┬────────────┐
        │            │           │           │            │
   init_task    VBAR_EL1    __fdt_pointer  kimage_   boot_cpu_
   task ptr     vectors     FDT blob       voffset   mode flag
        │            │           │           │            │
   init_cpu_   Set VBAR    str_l x21,  sub x4,x4,  set_cpu_
   task macro   msr vbar   __fdt_ptr   x0; str_l   boot_mode
        │            │           │           │            │
        └────────────┴───────────┴───────────┴────────────┘
                                 │
                         __primary_switched
                         (must set all this up
                          before bl start_kernel)
```

---

## The `init_task` Thread Info

At `start_kernel` entry, `init_task` (PID 0) is the current task:
```c
// init/init_task.c:
struct task_struct init_task
#ifdef CONFIG_ARCH_TASK_STRUCT_ON_STACK
    __init_task_data
#endif
= {
    .__state        = 0,
    .stack          = &init_thread_info,  ← init_stack region
    .usage          = REFCOUNT_INIT(2),
    .flags          = PF_KTHREAD,
    .prio           = MAX_PRIO - 20,
    .normal_prio    = MAX_PRIO - 20,
    .policy         = SCHED_NORMAL,
    .cpus_ptr       = &init_task.cpus_mask,
    .cpus_mask      = CPU_MASK_ALL,
    .comm           = INIT_TASK_COMM,    ← "swapper"
    .thread         = INIT_THREAD,
    .thread_pid     = &init_struct_pid,
    // ... many more fields
};
```

`init_cpu_task` in `__primary_switched` set `SP_EL0 = &init_task`, which is
how `current` (which reads `SP_EL0`) returns the init task pointer in C code.

---

## Confirming Everything Works: `set_task_stack_end_magic`

The very first call in `start_kernel`:
```c
void __init start_kernel(void)
{
    set_task_stack_end_magic(&init_task);
    ...
}

// include/linux/sched/task_stack.h:
static inline void set_task_stack_end_magic(struct task_struct *tsk)
{
    unsigned long *stackend = end_of_stack(tsk);
    *stackend = STACK_END_MAGIC;    // 0x57AC6E9DUL
}
```

This writes `STACK_END_MAGIC` to the lowest 8 bytes of the init stack.
If the stack ever overflows into this region, the stack overflow detector
(`task_stack_end_corrupted`) will find the corrupted magic value.

This is only possible because `init_cpu_task` correctly set up the stack
pointer to point into the valid `init_stack` region in `__primary_switched`.

---

## Assembly Perspective: The Last Moment of Assembly Code

```asm
// The last assembly instructions before C takes over:
    bl      finalise_el2        // last EL2 configuration
    bl      start_kernel        // JUMP to C world
    
    ASM_BUG()                   // should never reach here
    
    // After bl start_kernel, every instruction that runs is compiled C code
    // The CPU instruction pointer (PC) will never return to this assembly code
    // This is the final moment where assembly has control
    
// NOTE: If you're looking at a stack trace from a kernel panic in start_kernel,
// the outermost frame on CPU 0 will be __primary_switched → start_kernel.
// That's how you know you're looking at the primary boot CPU.
```

The `bl start_kernel` instruction is the ceremony of handing the CPU from
handcrafted, register-level, no-safety-net assembly code to the
C runtime with its abstractions, safety checks, and data structures.

Everything in `__primary_switched` — all 9 operations we've documented — exists
to make that one `bl` instruction safe and correct.

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