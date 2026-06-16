# Register State Transitions

This document tracks the major architectural state before and after `__cpu_setup`.

## Before `__cpu_setup`

You should assume:

- early boot context is active
- MMU-on control has not yet been committed for the next stage
- translation policy registers may not yet hold the intended Linux EL1 values

## After `__cpu_setup`

- `MAIR_EL1` holds Linux-selected attribute encodings
- `TCR_EL1` holds Linux-selected translation control policy
- `TCR2_EL1` may hold extension policy
- PMU and AMU EL0 access have been disabled if present
- `x0` holds the target MMU-on `SCTLR_EL1` value

## After `__enable_mmu`

- `TTBR0_EL1` and `TTBR1_EL1` are installed
- `SCTLR_EL1` is written
- translation becomes active
- execution continues under the intended MMU regime
