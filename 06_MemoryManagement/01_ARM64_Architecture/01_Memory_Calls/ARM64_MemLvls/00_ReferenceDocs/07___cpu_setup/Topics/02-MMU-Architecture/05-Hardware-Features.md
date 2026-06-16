# Hardware Features Used by `__cpu_setup`

Modern ARM64 CPUs do not all support the exact same optional features. `__cpu_setup` adapts at runtime.

## Feature families checked in this function

### VA52 and LPA2
If the kernel is configured for wider virtual addressing and the CPU supports it, the function adjusts `TCR_EL1` accordingly.

### HAFDBM and HAFT
The function reads `ID_AA64MMFR1_EL1` to see whether hardware can automatically manage access flags, and whether advanced fault-tracking support exists.

### PIE
The function reads `ID_AA64MMFR3_EL1` for S1PIE support. If supported, it programs permission-indirection registers.

### TCR2 support
The function also checks whether the CPU supports the extended translation register interface before writing `TCR2_EL1`.

## Why runtime checks matter

A Linux kernel image may support several ARM64 feature levels. Runtime ID registers let the kernel select the correct behavior on the actual CPU that booted.

## Important lesson

Some branches in `__cpu_setup` only exist if the kernel was built with support for the feature. Inside those branches, the final decision may still depend on what the running CPU reports.
