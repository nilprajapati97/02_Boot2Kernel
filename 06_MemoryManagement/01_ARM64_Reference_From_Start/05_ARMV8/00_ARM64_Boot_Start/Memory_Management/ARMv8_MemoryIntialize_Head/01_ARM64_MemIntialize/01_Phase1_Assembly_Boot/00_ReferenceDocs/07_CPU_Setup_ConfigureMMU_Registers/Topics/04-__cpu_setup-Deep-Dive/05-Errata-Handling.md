# Errata Handling

Early boot code must sometimes work around known CPU bugs.

## The errata hook in `__cpu_setup`

`__cpu_setup` calls `tcr_clear_errata_bits tcr, x9, x5`.

This macro lives in `arch/arm64/include/asm/assembler.h`.

## What it does conceptually

- read the CPU identification value from `MIDR_EL1`
- compare against known affected CPU patterns
- clear translation-control bits that trigger problematic behavior on those CPUs

## Why this happens before writing `TCR_EL1`

The kernel wants the final committed value of `TCR_EL1` to already include the workaround. That way the CPU never runs with a known-bad control setting.

## Beginner lesson

Early boot register programming is not just architecture design. It is also real-world hardware engineering. The code must respect both the architecture manual and the behavior of actual silicon.
