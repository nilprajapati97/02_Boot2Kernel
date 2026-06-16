# Hardware Update Setup

This document focuses on the optional hardware-managed behaviors configured by `__cpu_setup`.

## Hardware Access Flag update

When `ID_AA64MMFR1_EL1.HAFDBS` reports support, the kernel sets `TCR_EL1_HA`.

Meaning:
- the CPU can update access-flag state in hardware
- Linux does not need to rely entirely on software-managed access tracking for this feature

## HAFT path

If the feature level is high enough and the kernel is built with support, `TCR2_EL1_HAFT` is added.

This is an advanced extension beyond the baseline hardware access-flag capability.

## PIE path

If the CPU reports S1PIE support:

- `PIRE0_EL1` is programmed
- `PIR_EL1` is programmed
- `TCR2_EL1_PIE` is set in the working `tcr2` value

## Why group these together

All of these paths reflect the same pattern:

1. detect feature support at runtime
2. add the right control bits
3. write the extended register state only when supported

## Beginner takeaway

`__cpu_setup` does not assume all ARM64 CPUs are identical. It upgrades the translation policy when the CPU can safely support the extra capability.
