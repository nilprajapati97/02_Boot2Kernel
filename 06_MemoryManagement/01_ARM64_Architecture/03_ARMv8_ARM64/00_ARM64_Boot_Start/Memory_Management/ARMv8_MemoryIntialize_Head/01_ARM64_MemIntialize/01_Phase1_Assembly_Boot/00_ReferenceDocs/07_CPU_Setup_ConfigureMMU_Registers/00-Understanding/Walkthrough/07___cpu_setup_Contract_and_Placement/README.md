# 07 `__cpu_setup` Contract And Placement

`__cpu_setup` is the early EL1 setup routine that makes the CPU architecturally ready for Linux's stage-1 translation regime.

## What It Does

- invalidates stale local EL1 TLB state
- resets a small set of inherited control/debug registers
- builds and programs `MAIR_EL1`
- builds and programs `TCR_EL1`
- conditionally prepares `TCR2_EL1` and permission-indirection registers
- returns the final `SCTLR_EL1` value in `x0`

## What It Explicitly Does Not Do

- it does not load `TTBR0_EL1`
- it does not load `TTBR1_EL1`
- it does not perform the final write that turns `SCTLR_EL1.M` live

Those actions belong to `__enable_mmu`.

## Why It Lives In `.idmap.text`

The kernel cannot rely on the final higher-half virtual address layout while it is still preparing the registers that define that layout. So the code participating in the handoff must be executable under an identity mapping before and during the switch.

This is a mechanical requirement, not only a stylistic choice.

## The Core Design Split

Linux intentionally separates two phases:

1. build and program the translation regime metadata
2. install table bases and commit the final `SCTLR_EL1` write

That separation makes the sequence safer and easier to reuse for boot, secondary startup, and resume.