# `start_kernel` Handoff — Interview Q&A

---

## Q1: What is the purpose of `bl start_kernel` vs `b start_kernel`?

**A:** `bl start_kernel` saves the return address in `x30` (link register), while
`b start_kernel` does not. Even though `start_kernel` is designed to never return,
using `bl` is the correct choice because:

1. **Safety net**: The `ASM_BUG()` after `bl start_kernel` is reachable via `ret`
   if `start_kernel` somehow returns. A kernel panic with stack trace is produced,
   helping debug the catastrophic failure. Using `b` would lose the return address
   and potentially crash without diagnostics.

2. **Stack trace accuracy**: The `x30` value set by `bl` is the return address that
   stack unwinders use to identify the calling frame. When debugging, you can see
   that `start_kernel` was called from `__primary_switched`.

3. **Convention**: C function calls are made with `bl`. Using `b` for a C function
   entry is unconventional and confusing to readers.

---

## Q2: What global variables does `__primary_switched` set up, and why are they needed by `start_kernel`?

**A:** Three critical global variables:
1. `__fdt_pointer`: Physical address of the device tree blob. `setup_arch` calls
   `setup_machine_fdt(__fdt_pointer)` to parse memory maps, CPU topology,
   peripheral descriptions. Without this, the kernel doesn't know its hardware.

2. `kimage_voffset`: Virtual-minus-physical offset of the kernel image. Used by
   `__pa()` and `__va()` macros throughout `start_kernel`. `paging_init()` uses
   this to create the linear map. KASLR-enabled kernels need this since the
   offset varies each boot.

3. `__boot_cpu_mode[0]`: EL1 or EL2 boot mode. `kvm_arch_init()` (called from
   KVM module init) checks `is_hyp_mode_available()` which reads this value to
   decide if KVM can be enabled.

---

## Q3: Why does `start_kernel` use `asmlinkage` and what does it mean on ARM64?

**A:** `asmlinkage` tells GCC to pass function arguments via the stack (not
registers). On x86-32 this was crucial because the default calling convention
used registers, but assembly code used the stack. On ARM64, AAPCS64 already
uses registers (x0-x7) for function arguments, so `asmlinkage` is largely a
no-op.

However, it remains on `start_kernel` for:
1. **Historical compatibility**: Consistent with x86 port
2. **Documentation**: Signals "this function is called from assembly, not C"
3. **Safety**: If someone adds a parameter, it would be stack-passed on x86

The `__visible` attribute is more important on ARM64 — it prevents the linker
from removing `start_kernel` as an "unused" symbol (since no C code calls it
via a direct reference; it's called from assembly).

---

## Q4: What is `init_task` and what is its role after `start_kernel` begins?

**A:** `init_task` (also called "swapper/0") is the statically allocated task
structure for CPU 0. It was pointed to by `SP_EL0` in `__primary_switched`
(via `init_cpu_task`), making it the `current` task.

Lifecycle:
1. **Assembly phase**: `init_task` is a static structure; `init_cpu_task` sets
   `SP_EL0 = &init_task` making it "current"
2. **start_kernel phase**: runs ON `init_task`'s stack; `init_task` is the
   executing context for all of `start_kernel`
3. **rest_init phase**: `init_task` forks PID 1 (kernel_init) and PID 2 (kthreadd)
4. **Idle phase**: `init_task` becomes the idle task for CPU 0, runs `cpu_idle_loop`
   (executing `WFI` or `WFE` when no work is available)

`init_task` is permanent — it's never freed, never exits, runs forever as the
idle task. Its PID is 0, it's the ancestor of all other tasks.

---

## Q5: How does `start_kernel` handle the case where something in `__primary_switched` failed silently?

**A:** `start_kernel` has early sanity checks:
1. `set_task_stack_end_magic(&init_task)`: Verifies the stack pointer is within
   `init_stack` bounds. If `init_cpu_task` set SP incorrectly, this would detect it.
2. `cpu_probe()` (in `setup_arch`): Reads CPU ID registers — if registers are
   inaccessible (wrong EL), this fails visibly.
3. `early_fixmap_init()`: Tests page table manipulation — if `kimage_voffset`
   was wrong, VA-to-PA translations would fail here.
4. `check_and_switch_context()`: Tests ASID/VMID allocation — if TTBR registers
   are wrong, this crashes.

However, there's no explicit "validate everything from __primary_switched" call.
The kernel relies on the correctness of `__primary_switched` by design. If
`TPIDR_EL1` was set wrong, the first `this_cpu_read()` would return garbage,
and the kernel would quickly panic with a confusing error message.

---

## Q6: After `bl start_kernel`, can the kernel ever return to ARM64 assembly as the primary execution context?

**A:** Not to `__primary_switched` specifically, but assembly code IS called
throughout the kernel:
1. **Exception handlers**: C code flows into assembly when exceptions fire
   (defined in `arch/arm64/kernel/entry.S`)
2. **Context switches**: `cpu_switch_to` in assembly saves/restores task contexts
3. **SMP bringup**: New CPUs enter via assembly `secondary_entry`
4. **WFI idle**: The idle task calls assembly `cpu_do_idle` → `wfi` instruction

But `__primary_switched` itself is never revisited. The boot code in
`arch/arm64/kernel/head.S` after `start_kernel` entry is "dead code" from
the execution perspective — the instruction pointer never returns there.
It exists only as data (mapped in .text) but is never fetched as instructions
after the initial boot.

---

## Q7: What would happen if `bl start_kernel` were replaced with `bl start_kernel; bl start_kernel`?

**A:** The second `bl start_kernel` would never execute because `start_kernel`
never returns (it calls `cpu_startup_entry` which runs an infinite idle loop).

If by some hypothetical the first `start_kernel` returned, the second call
would re-execute `start_kernel`. This would:
1. Call `set_task_stack_end_magic` again — OK, idempotent
2. Call `smp_setup_processor_id` again — might cause confusion
3. Call `setup_arch` again — would likely re-parse FDT, re-allocate memblock
   nodes (double allocation → corruption!)
4. Call `mm_init` again — double-initialize the slab allocator → guaranteed panic

So `start_kernel` is NOT idempotent — calling it twice would cause severe
memory corruption. The `ASM_BUG()` safety net after the single `bl start_kernel`
prevents this scenario by ensuring a controlled crash if `start_kernel` ever
unexpectedly returns.

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