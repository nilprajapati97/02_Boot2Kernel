bl __cpu_setup
  ├── TLB flush (vmalle1)
  ├── Reset CPACR, MDSCR, PMU, AMU
  ├── Program MAIR_EL1  (memory types)
  ├── Program TCR_EL1   (VA layout, granule, caches)
  ├── Optionally TCR2_EL1, PIE registers
  └── Return x0 = SCTLR_EL1_MMU_ON (MMU not ON yet!)

b __primary_switch
  ├── __enable_mmu  → writes x0 to SCTLR_EL1 → MMU ON
  ├── __pi_early_map_kernel → kernel virtual mapping established
  └── br to __primary_switched (now in virtual address space)
       └── start_kernel()



Main block: head.S

1. Inputs to __primary_switch
- From earlier code, x0 already contains the SCTLR value prepared by __cpu_setup (MMU-on configuration), x20 has boot mode, and x21 has FDT pointer.
- Reference where __cpu_setup returns SCTLR value: proc.S

2. Prepare page table roots for MMU enable
- Instructions:
  - adrp x1, reserved_pg_dir
  - adrp x2, __pi_init_idmap_pg_dir
- CPU side:
  - Loads physical-address-based pointers (page-aligned via adrp) into registers.
- Memory side:
  - x1 will become TTBR1_EL1 root (kernel/global half).
  - x2 will become TTBR0_EL1 root (idmap / low mapping used for transition).
- Meaning of reserved_pg_dir: it is intentionally a "safe" TTBR0 setup used to block normal TTBR0 translations later as well. Kernel comment: mmu_context.h

3. Turn on MMU using __enable_mmu
- Call site: head.S
- Function: head.S
- CPU side inside __enable_mmu:
  - Verifies translation granule capability.
  - Writes TTBR0_EL1 from x2 and TTBR1_EL1 from x1.
  - Applies SCTLR_EL1 via set_sctlr_el1.
- Memory side:
  - Address translation becomes active at EL1.
  - Instruction/data accesses now go through page tables.
  - Barriers/serialization in set_sctlr ensure the switch is architecturally visible before continuing.
- Macro reference: assembler.h

4. Re-establish an early stack under translated addressing
- Instructions:
  - adrp x1, early_init_stack
  - mov sp, x1
  - mov x29, xzr
- CPU side:
  - Stack pointer is reset to a known early stack VA-compatible location.
  - Frame pointer cleared.
- Memory side:
  - Subsequent stack pushes/loads are through MMU translation now.

5. Build early kernel mappings and relocate
- Call: bl __pi_early_map_kernel at head.S
- Implementation body (PI C path): early_map_kernel in map_kernel.c
- CPU side:
  - Passes boot status (x20) and FDT pointer (x21).
  - Runs early C logic to finalize mapping decisions (VA bits, KASLR offset, feature overrides).
- Memory side:
  - Clears early BSS and init page-table area.
  - Maps FDT temporarily.
  - Constructs kernel virtual mapping (map_kernel).
  - Chooses/adjusts virtual placement (including KASLR when enabled).

6. Jump to post-switch virtual execution point
- Instructions:
  - ldr x8, =__primary_switched
  - adrp x0, KERNEL_START
  - br x8
- CPU side:
  - Indirect branch to __primary_switched continuation.
  - x0 carries physical KERNEL_START for later voffset calculation.
- Memory side:
  - Execution now continues in the fully mapped kernel virtual world (not just minimal idmap transition path).
- Next stage starts at: head.S

Quick mental model
- __cpu_setup: programs translation rules (MAIR/TCR/etc), returns MMU-on SCTLR value.
- __primary_switch: loads table roots, flips MMU on, creates final early kernel mappings, then branches into virtualized kernel execution path.

