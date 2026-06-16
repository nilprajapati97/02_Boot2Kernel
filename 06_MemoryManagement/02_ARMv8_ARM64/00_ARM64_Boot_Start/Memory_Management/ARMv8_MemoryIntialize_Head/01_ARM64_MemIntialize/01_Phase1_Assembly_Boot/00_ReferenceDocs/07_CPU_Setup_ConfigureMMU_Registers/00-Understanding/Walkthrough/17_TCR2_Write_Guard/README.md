# 17 `TCR2_EL1` Write Guard

Linux computes `tcr2` in a general-purpose register, but it does not write `TCR2_EL1` unconditionally.

## The Guard

The code re-reads `ID_AA64MMFR3_EL1`, checks whether `TCRX` support is present, and only then executes the write to `REG_TCR2_EL1`.

## Why This Matters

`TCR2_EL1` is not guaranteed to exist on all CPUs. Writing an unimplemented system register is not something the early boot path may assume is safe.

## Design Insight

This is a classic arm64 early-boot pattern:

- compute candidate control state in a normal register
- probe architectural support explicitly
- touch the system register only if the CPU says the register exists

That pattern lets one kernel image support a broad CPU population without splitting the boot logic into totally separate binaries.