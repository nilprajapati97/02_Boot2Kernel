# `__primary_switched` — Full Assembly Walkthrough

**Source:** `arch/arm64/kernel/head.S`  
**Context:** The first function executed entirely in kernel virtual address space

---

## 0. Function Signature in C Terms

```c
// Never returns. Calls start_kernel().
static void __noreturn __primary_switched(
    phys_addr_t boot_phys_offset,   // x0 = offset from link address to load address
    unsigned long fdt_ptr,          // x21 = FDT PA (preserved callee-saved)
    unsigned long cpu_boot_mode,    // x20 = BOOT_CPU_MODE (preserved callee-saved)
    ...
);
```

In assembly, parameters come from the preserved registers x20 and x21 (callee-saved
through the call chain). x0 may hold a value from `__pi_early_map_kernel`.

---

## 1. Complete Annotated Assembly

```asm
// arch/arm64/kernel/head.S
SYM_FUNC_START_LOCAL(__primary_switched)
    // ── (1) BTI landing pad ──────────────────────────────────────
    // (With CONFIG_ARM64_BTI_KERNEL=y, SYM_FUNC_START emits BTI j/jc)
    // This is the first instruction executed in kernel VA space.

    // ── (2) Stack setup ──────────────────────────────────────────
    adr_l   x4, init_thread_union
    // x4 = VA of init_thread_union (the initial kernel stack / task_struct)
    // init_thread_union is in .bss (or .data) and is VA-accessible now.
    
    add     sp, x4, #THREAD_SIZE
    // SP = VA of top of init_thread_union + 16KB
    // = bottom of stack (ARM64 stack grows down from high to low address)
    // SP is now pointing to a valid kernel VA stack.
    // This replaces the early_init_stack (which was physical/identity-mapped).

    // ── (3) Set current task pointer ─────────────────────────────
    msr     sp_el0, x4
    // SP_EL0 = &init_thread_union = &init_task (task_struct is at offset 0)
    // In kernel mode, SP_EL0 is used as the 'current' pointer.
    // current = (struct task_struct *)SP_EL0
    // This allows: current->pid, current->comm, etc.

    // ── (4) Frame pointer reset ───────────────────────────────────
    mov     x29, xzr
    // x29 (frame pointer) = 0
    // Marks the beginning of the kernel call stack.
    // Unwinder sees fp=0 and stops here (top of backtrace).
    // This prevents the unwinder from walking into pre-MMU garbage.

    // ── (5) Link register setup ───────────────────────────────────
    mov     x30, xzr
    // LR = 0 (no return address — we came here via br, not bl)
    // If start_kernel ever tries to return, it would jump to VA 0 → fault.
    // This is an intentional sentinel: __primary_switched does NOT return.

    // ── (6) Exception vector table ───────────────────────────────
    adr_l   x8, vectors
    msr     vbar_el1, x8
    // VBAR_EL1 = VA of the kernel exception vector table.
    // From this point, exceptions (interrupts, data aborts, system calls)
    // are dispatched via the proper kernel vector table.
    // Before this, any exception would go to the early stub vectors
    // (which panic immediately, as they have no real handler).

    // ── (7) BSS zeroing ───────────────────────────────────────────
    ldr     x4, =__bss_start
    ldr     x5, =__bss_stop
    // x4 = VA of BSS section start
    // x5 = VA of BSS section end
    
    bl      __pi_memset
    // Call PI memset to zero the BSS.
    // BSS was not zeroed by the bootloader (the kernel zeroes it itself).
    // After this, all global C variables with initial value 0 are correct.
    
    // NOTE: This also zeroes early_init_stack (which is in BSS).
    // That's safe because we've already switched to init_thread_union stack
    // in step (2) above.

    // ── (8) Store BOOT_CPU_MODE ───────────────────────────────────
    str_l   w20, __boot_cpu_mode, x5
    // __boot_cpu_mode = (u32)x20 = BOOT_CPU_MODE (EL1=0xe11 or EL2=0xe12)
    // |BOOT_CPU_FLAG_E2H if running in VHE mode
    // Used later by: is_hyp_mode_available(), arm_smccc_1_1_invoke()
    // KVM reads this to decide nVHE vs VHE KVM mode.

    // ── (9) EL2 setup (if booted at EL2 / VHE) ───────────────────
    ldr     x8, =vectors
    bl      el2_setup
    // el2_setup(vectors_va):
    //   If booted at EL2 (x20 & BOOT_CPU_FLAG_E2H), set up EL2 registers.
    //   With VHE: VBAR_EL2 = vectors (EL2 also uses kernel vectors)
    //   Without VHE (nVHE KVM): set up hyp stubs
    //   If not at EL2: no-op
    // Return value: mode flags for the caller
    
    // ── (10) Call start_kernel ────────────────────────────────────
    bl      start_kernel
    // NEVER RETURNS.
    // start_kernel → rest_init → cpu_startup_entry(CPUHP_ONLINE)
    // The bootstrap task becomes the idle task (PID 0) and loops forever.

    // ── (11) Unreachable ─────────────────────────────────────────
    ASM_BUG()
    // If start_kernel somehow returns (it shouldn't), this generates a BRK
    // instruction which triggers a debug exception → panic.

SYM_FUNC_END(__primary_switched)
```

---

## 2. Register State on Entry to `__primary_switched`

| Register | Value | Source |
|---|---|---|
| x0 | `boot_phys_offset` or unused | From `__pi_early_map_kernel` return |
| x20 | `BOOT_CPU_MODE` | Set in `el2_setup`, preserved across callees |
| x21 | PA of FDT | From boot protocol, preserved across callees |
| x29 | Frame pointer (stale, about to be zeroed) | From `__primary_switch` |
| x30 | Return address to `__primary_switch` (stale) | `ldr x8; br x8` used, not `blr` |
| SP | VA of `early_init_stack` top | Set in `__primary_switch` after MMU enable |

---

## 3. Stack Transition in Detail

```
Before __primary_switched:
  SP = VA of early_init_stack top
     = VA in .bss section (early_init_stack + SZ_4K)
     = identity-mapped equivalent of PA

After `add sp, x4, #THREAD_SIZE`:
  SP = VA of init_thread_union + THREAD_SIZE (16KB)
  init_thread_union = struct {
      union {
          struct task_struct task;    // PID 0 (swapper)
          unsigned long stack[...];   // The actual stack storage
      };
  };
  Stack grows DOWN from SP: first push = init_thread_union + 16KB - 16
```

After BSS zeroing, `early_init_stack` (in .bss) is zeroed. The old stack
frames from the pre-MMU world are gone. The call stack effectively begins fresh
at `__primary_switched`.

---

## 4. `__boot_cpu_mode` — Details

```c
// arch/arm64/kernel/head.S (or head-common.S)
SYM_DATA(__boot_cpu_mode, .word 0, .word 0)
// Two 32-bit words:
// [0] = CPU boot mode of primary CPU (set in __primary_switched)
// [1] = CPU boot mode of secondary CPUs (set later in secondary_switched)
```

Usage:
```c
// arch/arm64/include/asm/virt.h
static inline bool is_hyp_mode_available(void)
{
    return (__boot_cpu_mode[0] == BOOT_CPU_MODE_EL2 &&
            __boot_cpu_mode[1] == BOOT_CPU_MODE_EL2);
}
```

Both words must agree (all CPUs booted at EL2) for KVM to be fully available.

---

## 5. Setting VBAR_EL1 — Switching Exception Vectors

Before `__primary_switched`, exception vectors were set to `__boot_vectors` in
`head.S` — a minimal stub that panics on any exception.

After `msr vbar_el1, x8` (x8 = `vectors`):
```
VBAR_EL1 = VA of vectors table

vectors table (arch/arm64/kernel/entry.S):
  0x000: el1t_sync_invalid
  0x080: el1t_irq_invalid
  0x100: el1t_fiq_invalid
  0x180: el1t_error_invalid
  0x200: el1h_sync           ← Data abort, instruction abort, etc. (EL1h)
  0x280: el1h_irq            ← IRQ at EL1 (kernel interrupt)
  0x300: el1h_fiq
  0x380: el1h_error
  0x400: el0_sync            ← Syscall (SVC), EL0 abort
  0x480: el0_irq
  ...
```

After this, the kernel can handle real exceptions (including `start_kernel`'s
early IRQ setup, ACPI/DT parsing, etc.).

---

## 6. BSS Zeroing — Why It Must Happen Now

BSS zeroing is deferred until `__primary_switched` for several reasons:

1. **BSS is not zeroed by the bootloader.** The Linux boot protocol says the
   kernel is responsible for clearing BSS. (Contrast with C runtime startup,
   where the C startup code clears BSS before `main()`.)

2. **Cannot zero BSS before MMU enable.** BSS is at kernel VAs, which are not
   accessible before `swapper_pg_dir` is active. Attempting to write to BSS
   VAs before MMU would write to wrong physical addresses or fault.

3. **BSS contains `early_init_stack`.** Zeroing BSS also clears the old pre-MMU
   stack. This is safe because we've already switched to `init_thread_union`.

4. **Stack switch must precede BSS zero.** If BSS were zeroed before
   `init_thread_union` is set up as the stack, the function call to
   `__pi_memset` would use `early_init_stack` for the memset call frame —
   which is about to be zeroed. This would corrupt the stack! The order
   matters: set SP first, then zero BSS.

---

## 7. What `start_kernel` Does and Why It Never Returns

```c
// init/main.c
asmlinkage __visible void __init __no_sanitize_address start_kernel(void)
{
    char *command_line;
    char *after_dashes;

    set_task_stack_end_magic(&init_task);  // Stack canary for PID 0
    smp_setup_processor_id();
    debug_objects_early_init();

    init_vmlinux_build_id();

    cgroup_init_early();

    local_irq_disable();
    early_boot_irqs_disabled = true;

    /*
     * Interrupts are still disabled. Do necessary setups, then
     * enable them.
     */
    boot_cpu_init();
    page_address_init();
    pr_notice("%s", linux_banner);
    early_security_init();
    setup_arch(&command_line);   // arch-specific: memory map, DTB parse
    setup_boot_config();
    setup_command_line(command_line);
    setup_nr_cpu_ids();
    setup_per_cpu_areas();
    smp_prepare_boot_cpu();
    boot_cpu_hotplug_init();

    build_all_zonelists(NULL);
    page_alloc_init();

    // ... hundreds more init calls ...

    arch_call_rest_init();      // → rest_init()
    // rest_init() creates kernel threads:
    //   kernel_init (PID 1, eventually becomes /sbin/init or initramfs)
    //   kthreadd  (PID 2, kernel thread daemon)
    // Then calls cpu_startup_entry(CPUHP_ONLINE) which becomes the idle loop.
    // The idle loop is: while(1) { wfi; }  (Wait For Interrupt)
}
```

`start_kernel` ends by calling `rest_init()` which calls
`cpu_startup_entry(CPUHP_ONLINE)`. This function enters the idle loop and
**never returns**. The primary CPU (PID 0, swapper) is now the idle task.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
This document describes a stage in the ARMv8-A Linux ARM64 boot path. ARMv8-A is the 64-bit ARM architecture (AArch64 execution state) introduced with the ARM Cortex-A53/A57 generation. Key architectural features relevant to boot:
- Exception levels: EL0 (user), EL1 (OS kernel), EL2 (hypervisor), EL3 (secure monitor).
- Two-stage translation: TTBR0_EL1 (user/low VA) and TTBR1_EL1 (kernel/high VA).
- System registers accessed via MRS/MSR instructions (not memory-mapped).
- PSTATE: condition flags + CPU mode + interrupt mask bits.
- Mandatory ISB after system register writes that affect instruction fetch.

### Kernel Perspective (Linux ARM64)
The Linux ARM64 boot path follows this sequence:
  stext (head.S) -> __primary_switch -> __pi_early_map_kernel -> __enable_mmu
  -> __primary_switched -> start_kernel -> setup_arch -> paging_init
Each stage initializes one more layer of the memory system. Before start_kernel, all memory management is done with physical addresses or the early identity/kernel maps. After paging_init(), the full kernel virtual memory map is active.

### Memory Perspective (ARMv8 Memory Model)
The ARMv8 memory model (based on the ARM ARM's "Arm Memory Model" chapter) defines:
- Normal memory: cacheable, reorderable, speculatable. Used for DRAM (kernel code, data, stack, heap).
- Device memory: non-cacheable, strictly ordered. Used for MMIO (UART, GIC, etc.).
- Barriers: DSB (Data Synchronization Barrier), DMB (Data Memory Barrier), ISB (Instruction Synchronization Barrier) enforce ordering guarantees.
At boot, the kernel transitions from a world where every address is physical (pre-MMU) to the full ARMv8 virtual memory model where TTBR0 and TTBR1 map the user and kernel address spaces respectively.