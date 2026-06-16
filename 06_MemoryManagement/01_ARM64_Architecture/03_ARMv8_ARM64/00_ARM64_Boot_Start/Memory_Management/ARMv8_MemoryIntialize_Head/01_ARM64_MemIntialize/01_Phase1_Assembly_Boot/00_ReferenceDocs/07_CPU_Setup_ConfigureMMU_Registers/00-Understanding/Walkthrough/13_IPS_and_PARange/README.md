# 13 `IPS` And `PARange`

Virtual address size and physical address size are separate questions.

## What Linux Computes Here

The helper `tcr_compute_pa_size` reads the CPU's implemented physical-address range from `ID_AA64MMFR0_EL1.PARange`, clamps it to what Linux is prepared to use, and inserts the resulting encoding into the `IPS` field of `TCR_EL1`.

## Why `IPS` Matters

The `IPS` field tells the hardware what output physical address size the stage-1 translation regime expects. If this field is wrong, the regime can become invalid, suboptimal, or inconsistent with the actual implementation.

## Hardware-Level Interpretation

This step translates hardware discovery into a live MMU policy choice. It is the moment where Linux stops treating `PARange` as raw information and turns it into committed translation-control state.

## Failure Mode If Wrong

- page-table entries may not be interpreted against the correct PA width
- a legal physical address might become unreachable
- an architecturally inconsistent translation regime can lead to faults or undefined behavior