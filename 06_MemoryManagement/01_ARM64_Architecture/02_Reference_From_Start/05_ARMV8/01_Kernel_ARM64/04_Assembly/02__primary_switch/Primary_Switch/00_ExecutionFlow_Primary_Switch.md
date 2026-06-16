## Step-by-step execution after `adrp x1, reserved_pg_dir`

Here is the full `__primary_switch` sequence:

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    adrp    x1, reserved_pg_dir         // (1) x1 = phys addr of reserved_pg_dir (TTBR1)
    adrp    x2, __pi_init_idmap_pg_dir  // (2) x2 = phys addr of idmap (TTBR0)
    bl      __enable_mmu                // (3) Turn on the MMU

    adrp    x1, early_init_stack        // (4) x1 = early stack top
    mov     sp, x1                      // (5) Switch to the early stack
    mov     x29, xzr                    // (6) Clear frame pointer
    mov     x0, x20                     // (7) x0 = boot mode (from __cpu_setup)
    mov     x1, x21                     // (8) x1 = FDT pointer
    bl      __pi_early_map_kernel       // (9) Map & relocate the kernel

    ldr     x8, =__primary_switched     // (10) x8 = *virtual* addr of __primary_switched
    adrp    x0, KERNEL_START            // (11) x0 = __pa(KERNEL_START)
    br      x8                          // (12) Jump to virtual address space
SYM_FUNC_END(__primary_switch)
```

### What each step does

| Step | Instruction | Purpose |
|------|------------|---------|
| 1 | `adrp x1, reserved_pg_dir` | Loads the physical base address of `reserved_pg_dir` into x1 — this acts as a **temporary TTBR1_EL1** (kernel VA space mapping) because real kernel page tables don't exist yet |
| 2 | `adrp x2, __pi_init_idmap_pg_dir` | Loads the identity map page table into x2 for **TTBR0_EL1** |
| 3 | `bl __enable_mmu` | Validates granule support, loads TTBR0/TTBR1, then writes SCTLR_EL1 to **enable the MMU**. After return, the CPU is running with MMU on |
| 4–6 | stack setup | Re-establishes the early stack and clears the frame pointer |
| 7–9 | `bl __pi_early_map_kernel` | Builds the **real kernel page tables** and applies relocations (KASLR if enabled). This replaces the reserved placeholder |
| 10–12 | `ldr x8` / `br x8` | Performs an **absolute jump** to `__primary_switched` using its **virtual address** — the first instruction executing in the final kernel VA space |

### Key insight about `reserved_pg_dir`

`reserved_pg_dir` is intentionally an **empty/reserved** page directory. It's used as a placeholder for TTBR1 (kernel address space) just long enough to turn the MMU on. Only after `__pi_early_map_kernel` sets up the real mappings and the `br x8` jumps to the virtual address does the kernel run at its proper virtual addresses, eventually entering `__primary_switched` → `start_kernel`.

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