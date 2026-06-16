# Enter the C Kernel — `bl start_kernel`, the Point of No Return, and `ASM_BUG()`

**File**: `arch/arm64/kernel/head.S` — inside `__primary_switched`
**Instructions**:
```asm
bl      start_kernel    // Branch-with-link to the C kernel entry point
ASM_BUG()               // Unreachable: BRK #0x800 debug exception
```
**Perspective**: Boot Architecture / C Runtime Contract / System Invariants
**Style**: NVIDIA Boot Architecture / Google Android Platform Security

---

## 1. `bl start_kernel` — The Most Important Branch in the Kernel

```asm
bl  start_kernel
```

This single instruction is the **precise boundary** between:
- Assembly-language boot code (architecture-specific, position-independent)
- C-language kernel (architecture-independent, virtual address, full ABI)

Before this `bl`:
- MMU is ON with `swapper_pg_dir` (kernel virtual address space active)
- `init_task` stack is live (SP valid)
- VBAR_EL1 points to exception vectors (exceptions handled correctly)
- `__fdt_pointer` saved (FDT accessible to C code)
- `kimage_voffset` computed (KASLR offset known)
- CPU mode committed to `__boot_cpu_mode[]` (KVM eligibility decided)
- EL2/VHE promotion done (if applicable)
- KASAN early shadow mapped (all C code safe to execute)
- Frame pointer chain valid (x29 → FINAL sentinel)

After this `bl`, control enters `start_kernel` in `init/main.c` — a normal
C function executing in the full kernel virtual address space.

---

## 2. Why `bl` and Not `b`?

`bl` saves the return address in `x30` (LR):
```asm
bl start_kernel
// x30 = address of ASM_BUG() instruction (next instruction after bl)
```

Using `bl` instead of `b` (plain branch):
1. **AAPCS64 convention**: Calling a C function via `bl` is correct form.
   Compilers expect to be entered via `bl` (x30 contains return address).
   Using `b` would leave x30 with a stale value, and `start_kernel`'s prologue
   would save a meaningless return address in its frame record.
2. **Return address for backtraces**: x30 = `ASM_BUG()` address. When
   `start_kernel` builds its stack frame (`stp x29, x30, [sp, #-N]!`),
   it saves this return address. Stack unwinders will show:
   ```
   start_kernel+0x0
   0x<ASM_BUG address>   ← return address from __primary_switched
   ```
3. **Correctness**: If `start_kernel` ever returned (it shouldn't), execution
   would land on `ASM_BUG()` which immediately traps. The `bl` + `ASM_BUG()`
   pair is a safety net.

---

## 3. State Contract: What `start_kernel` Expects at Entry

`start_kernel` is a C function. AAPCS64 requires:
```
x0: first argument (but start_kernel takes void — x0 undefined, ignored)
SP: 16-byte aligned     ← guaranteed by epilogue
x29: valid FP           ← restored by epilogue to FINAL sentinel addr
x30: return address     ← set by bl to ASM_BUG() address

Memory:
  - Virtual address mappings: swapper_pg_dir active
  - init_task.stack: valid, SP within it
  - Exception vectors: VBAR_EL1 set
  - KASAN shadow: early-mapped (all valid)
```

`start_kernel` does NOT expect:
- Any C runtime initialization (no `.init_array`, no constructors)
- Global variable initialization (BSS is zeroed in `__primary_switch` by `memset_l`)
- Dynamic memory allocation (kmalloc not ready — happens inside `start_kernel`)

---

## 4. What `start_kernel` Does: The First 20 Steps

```c
// init/main.c
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
{
    char *command_line;
    char *after_dashes;

    set_task_stack_end_magic(&init_task);    // Canary at stack bottom
    smp_setup_processor_id();               // CPU 0
    debug_objects_early_init();             // Debug object allocator
    init_vmlinux_build_id();                // Build ID for crash dumps
    cgroup_init_early();                    // cgroup v1/v2 initialization
    local_irq_disable();                    // Interrupts off for early init
    early_boot_irqs_disabled = true;

    boot_cpu_init();                        // Mark CPU0 active in cpu_present_mask
    page_address_init();                    // Highmem page address table
    pr_notice("%s", linux_banner);          // "Linux version X.Y.Z ..."
    early_security_init();                  // LSM early hooks
    setup_arch(&command_line);             // ← MOST IMPORTANT: FDT, memory layout
    setup_boot_config();                    // Bootconfig from initrd
    setup_command_line(command_line);       // Parse cmdline
    setup_nr_cpu_ids();                     // How many CPUs
    setup_per_cpu_areas();                  // Per-CPU variable regions
    smp_prepare_boot_cpu();                 // SMP prep for boot CPU
    boot_cpu_hotplug_init();                // CPU hotplug init
    build_all_zonelists(NULL);              // Memory zone lists
    page_alloc_init();                      // Page allocator callbacks
    ...
    mm_init();                              // Full memory allocator
    ...
    rest_init();                            // Launch init process → never returns
}
```

`setup_arch()` is particularly significant — it calls `setup_machine_fdt(__fdt_pointer)`
which parses the Device Tree (built at `__fdt_pointer`) and builds the full
memory map, enabling `memblock` allocations.

---

## 5. `ASM_BUG()` — The Unreachable Trap

```asm
ASM_BUG()
```

This expands to:
```asm
// arch/arm64/include/asm/bug.h
brk  #BUG_BRK_IMM     // BUG_BRK_IMM = 0x800
```

`BRK #0x800` is a **synchronous debug exception**:
- Trapped by the exception vector at EL1 (VBAR_EL1 installed by `__primary_switched`)
- Exception class (ESR_EL1.EC) = 0b111100 = BRK instruction
- Exception handler calls `do_debug_exception()` → `bug_handler()`
- `bug_handler()` calls `panic("Oops - BUG: instruction bug")` or similar

The comment in the source is clear: `start_kernel` is **never expected to
return**. The `ASM_BUG()` after `bl start_kernel` is a defensive trap — if
`start_kernel` somehow returned (which would indicate a catastrophic bug),
the CPU would immediately trap instead of executing garbage code.

---

## 6. The Boot Is Complete: Assembly Boot Code Never Runs Again

After `bl start_kernel` fires:

```
Assembly boot path:           DORMANT (never re-entered for primary CPU)
  primary_entry               ← complete
  preserve_boot_args          ← complete
  init_kernel_el              ← complete
  __cpu_setup                 ← complete
  __primary_switch            ← complete (TTBR0/TTBR1 built, MMU on)
    __pi_early_map_kernel     ← complete
    __primary_switched:       ← complete (this function)
      init_cpu_task           ← complete
      vbar_el1                ← complete
      kimage_voffset          ← complete
      __fdt_pointer           ← complete
      set_cpu_boot_mode_flag  ← complete
      kasan_early_init        ← complete (if enabled)
      finalise_el2            ← complete
      bl start_kernel         ← *** TRANSFER OF CONTROL ***
      ASM_BUG()               ← never reached

C kernel:                     ACTIVE FOREVER
  start_kernel                ← running
    setup_arch
    mm_init
    sched_init
    rest_init
      kernel_init (PID 1)     ← init process
      kthreadd (PID 2)        ← kthread daemon
```

The entire assembly boot sequence — thousands of lines, dozens of functions,
built for one purpose — exists solely to bring the hardware to a state where
`start_kernel` can run safely. That state is achieved here.

---

## 7. The `__primary_switched` Function: Complete Overview

```
SYM_FUNC_START_LOCAL(__primary_switched)
  01. init_cpu_task x4, x5, x6      // init_task, SP, SP_EL0, TLS
  02. msr vbar_el1, x8              // Exception vectors active
  03. isb                           // Synchronize vector install
  04. stp x29, x30, [sp, #-16]!    // Calling frame established
  05. mov x29, sp                   // FP = SP
  06. str_l x21, __fdt_pointer      // FDT physical address saved globally
  07. adrp x4, _text                // Compute kimage_voffset
      sub  x4, x4, x0
      str_l x4, kimage_voffset      // VA-PA offset for KASLR committed
  08. mov x0, x20                   // Boot mode
      bl set_cpu_boot_mode_flag     // __boot_cpu_mode[] written
  09. bl kasan_early_init           // (if KASAN_GENERIC || SW_TAGS)
  10. mov x0, x20                   // Boot mode
      bl finalise_el2               // VHE promotion (if EL2 boot)
  11. ldp x29, x30, [sp], #16      // Epilogue: frame restored
  12. bl start_kernel               // ENTER C KERNEL — NO RETURN
  13. ASM_BUG()                     // Unreachable: BRK #0x800
SYM_FUNC_END(__primary_switched)
```

This is the end of the ARM64 boot path documentation series.
`start_kernel` marks the beginning of the architecture-independent kernel.
