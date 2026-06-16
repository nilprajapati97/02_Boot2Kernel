## `__primary_switch` — ARM64 Primary Boot Path

This function bridges the gap between early identity-mapped boot code and the fully virtual-addressed kernel. It runs with the MMU **off**, then enables it and jumps into virtual address space.

### Step-by-step breakdown

**1. Enable the MMU**
```asm
adrp    x1, reserved_pg_dir
adrp    x2, __pi_init_idmap_pg_dir
bl      __enable_mmu
```
- Loads the physical addresses of two page tables:
  - `reserved_pg_dir` → will become `TTBR1_EL1` (kernel virtual address space, upper half)
  - `__pi_init_idmap_pg_dir` → identity map for `TTBR0_EL1` (lower half, 1:1 phys→virt)
- Calls `__enable_mmu` which writes these into the TTBRs and sets `SCTLR_EL1.M`, turning the MMU **on**

**2. Switch to a temporary early stack**
```asm
adrp    x1, early_init_stack
mov     sp, x1
mov     x29, xzr
```
- Switches the stack pointer to `early_init_stack` (a statically allocated early boot stack)
- Clears the frame pointer (`x29 = 0`) to mark the base of the call chain for unwinding

**3. Map and relocate the kernel**
```asm
mov     x0, x20     // pass the full boot status
mov     x1, x21     // pass the FDT
bl      __pi_early_map_kernel
```
- `x20` holds the CPU boot mode (EL1/EL2 flags, set earlier by `init_kernel_el`)
- `x21` holds the physical address of the Flattened Device Tree (FDT), preserved since `primary_entry`
- `__pi_early_map_kernel` is a position-independent C function that:
  - Sets up the final kernel page tables
  - Applies KASLR relocation fixups if enabled
  - Maps the kernel image, FDT, and other early regions

**4. Jump into virtual address space**
```asm
ldr     x8, =__primary_switched    // absolute virtual address via literal pool
adrp    x0, KERNEL_START           // __pa(KERNEL_START)
br      x8
```
- `ldr x8, =...` loads the **virtual** address of `__primary_switched` (a linked-address constant, not PC-relative)
- `x0` is set to the physical address of `KERNEL_START`, passed as an argument to `__primary_switched`
- `br x8` performs the **"virtual address switch"** — the first instruction executed at a kernel virtual address rather than a physical/identity-mapped address

### Why `ldr` instead of `bl`?

`bl` is PC-relative (±128MB range). At this point, the code is running at a physical address, but `__primary_switched` lives at a high virtual address. Using `ldr x8, =symbol` fetches the absolute linked address from a literal pool, then `br x8` jumps to it unconditionally — crossing the physical-to-virtual boundary.

### Control flow summary

```
primary_entry
    → __cpu_setup      (processor init, TCR setup)
    → __primary_switch
        → __enable_mmu          (MMU on, identity map active)
        → __pi_early_map_kernel (kernel page tables + KASLR)
        → __primary_switched    (now running at virtual addresses)
            → start_kernel      (C kernel entry point)
```

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