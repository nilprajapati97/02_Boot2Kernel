# Function Overview

Source: `arch/arm64/mm/proc.S`

## Purpose

`__cpu_setup` initializes the processor-side EL1 memory-management controls needed for safe MMU activation.

## Inputs

There is no complex C-style argument list here. The function is called from early assembly, and it uses temporary general-purpose registers internally.

What it logically consumes is the surrounding machine state:

- current early boot execution context
- architectural feature information from ID registers
- compile-time kernel configuration choices

## Output

The function returns one key value in `x0`:

- `INIT_SCTLR_EL1_MMU_ON`

That returned constant is later written into `SCTLR_EL1` by the MMU-enable path.

## Side effects

The function writes these registers directly or conditionally:

- `CPACR_EL1`
- `MDSCR_EL1`
- `MAIR_EL1`
- `TCR_EL1`
- possibly `TCR2_EL1`
- possibly `PIRE0_EL1` and `PIR_EL1`
- possibly `PMUSERENR_EL0` and `AMUSERENR_EL0` through helper macros

## Non-goals

- it does not create the early page tables
- it does not load `TTBR0_EL1` or `TTBR1_EL1`
- it does not enable the MMU directly
