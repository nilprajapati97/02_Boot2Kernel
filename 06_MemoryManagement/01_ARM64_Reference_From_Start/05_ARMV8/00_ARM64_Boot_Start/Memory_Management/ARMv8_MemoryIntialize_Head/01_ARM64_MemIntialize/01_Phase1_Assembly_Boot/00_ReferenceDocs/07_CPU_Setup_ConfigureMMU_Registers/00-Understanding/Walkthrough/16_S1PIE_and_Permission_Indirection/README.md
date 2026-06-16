# 16 `S1PIE` And Permission Indirection

Linux next probes `ID_AA64MMFR3_EL1` for stage-1 permission indirection support.

## What The Code Does

If the CPU advertises `S1PIE`, Linux writes values into `PIRE0_EL1` and `PIR_EL1`, then sets `TCR2_EL1.PIE` in the temporary `tcr2` value.

## High-Level Meaning

Permission indirection means the architecture can introduce an extra interpretation layer for permissions instead of relying only on the classic direct reading of leaf-descriptor permission bits.

For a reader trying to understand boot code, the key points are:

- feature presence is runtime-detected
- extra system registers must be initialized before the mode is enabled
- `TCR2_EL1` is the enable point for the associated behavior

## Why Linux Handles It Here

This belongs in `__cpu_setup` because it is part of the architectural contract that must be correct before the MMU is turned on for Linux's translation regime.