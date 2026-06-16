# ARM64 Register Model

To understand `__cpu_setup`, separate registers into two groups.

## General-purpose registers

ARM64 has registers `x0` through `x30`.

In early assembly code, these are used as temporary working storage and argument passing registers.

Examples inside `__cpu_setup`:

- `x0` is used for the final return value.
- `x1` is used as a scratch register for debug and feature checks.
- `x15`, `x16`, and `x17` are temporarily named `tcr2`, `tcr`, and `mair`.
- `x5`, `x6`, and `x9` are used as scratch registers in macros and feature detection.

## System registers

These are architectural control registers accessed with `mrs` and `msr` instructions.

Examples used directly or indirectly by `__cpu_setup`:

- `CPACR_EL1`
- `MDSCR_EL1`
- `MAIR_EL1`
- `TCR_EL1`
- `TCR2_EL1`
- `ID_AA64MMFR1_EL1`
- `ID_AA64MMFR3_EL1`

## Why `.req` matters

In assembly, this code uses:

- `mair .req x17`
- `tcr .req x16`
- `tcr2 .req x15`

This does not create a new hardware register. It only gives a readable local name to an existing general-purpose register.

## Return-value convention

The function returns in `x0`. Here that returned value is `INIT_SCTLR_EL1_MMU_ON`, which is later written into `SCTLR_EL1` by the MMU enable path.
