# Kernel Source Map

This document maps the key Linux source files involved in understanding `__cpu_setup`.

## Primary function

### `arch/arm64/mm/proc.S`

Role:
- Defines `__cpu_setup`.
- Defines early control register defaults such as `MAIR_EL1_SET`, `TCR_TG_FLAGS`, `TCR_CACHE_FLAGS`, `TCR_SHARED`, and feature-conditional branches.

Important symbols:
- `__cpu_setup`
- `MAIR_EL1_SET`
- `TCR_TG_FLAGS`
- `TCR_KASLR_FLAGS`
- `TCR_CACHE_FLAGS`
- `TCR_SHARED`
- `TCR_MTE_FLAGS`
- `TCR_KASAN_SW_FLAGS`

## Boot flow caller

### `arch/arm64/kernel/head.S`

Role:
- Primary boot entry path.
- Secondary CPU boot path.
- Calls `init_kernel_el`, `__cpu_setup`, and later `__enable_mmu`.

Important symbols:
- `primary_entry`
- `init_kernel_el`
- `__primary_switch`
- `__enable_mmu`
- `secondary_startup`

## Helper macros

### `arch/arm64/include/asm/assembler.h`

Role:
- Provides helper macros used by early assembly code.

Important macros:
- `tcr_set_t0sz`
- `tcr_set_t1sz`
- `tcr_compute_pa_size`
- `tcr_clear_errata_bits`
- `reset_pmuserenr_el0`
- `reset_amuserenr_el0`
- `set_sctlr_el1`

## Register definitions

### `arch/arm64/include/asm/sysreg.h`

Role:
- Defines fields and constants for ARM64 system registers.

Important constants:
- `INIT_SCTLR_EL1_MMU_ON`
- `INIT_SCTLR_EL1_MMU_OFF`
- `SCTLR_ELx_M`
- `SCTLR_ELx_C`
- `SCTLR_ELx_I`
- `MAIR_ATTR_*`
- `ID_AA64MMFR*`

## Memory layout

### `arch/arm64/include/asm/memory.h`

Role:
- Defines `VA_BITS`, `VA_BITS_MIN`, `PAGE_OFFSET`, and kernel layout constants.

## Identity map and page-table geometry

### `arch/arm64/include/asm/kernel-pgtable.h`

Role:
- Defines `IDMAP_VA_BITS`, idmap levels, and early page-table calculations.

## Permission indirection

### `arch/arm64/include/asm/pgtable-prot.h`

Role:
- Defines `PIE_E0` and `PIE_E1` permission mapping constants.

## Follow-on MMU setup context

### `arch/arm64/mm/mmu.c`

Role:
- Provides later memory-management setup beyond the earliest assembly stage.

## Suggested cross-reading order

1. `head.S`
2. `proc.S`
3. `assembler.h`
4. `sysreg.h`
5. `memory.h`
6. `kernel-pgtable.h`
7. `pgtable-prot.h`
8. `mmu.c`
