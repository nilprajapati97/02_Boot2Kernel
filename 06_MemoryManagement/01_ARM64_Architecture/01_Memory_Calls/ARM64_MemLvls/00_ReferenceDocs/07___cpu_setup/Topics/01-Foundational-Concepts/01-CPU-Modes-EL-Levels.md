# CPU Modes and Exception Levels

ARM64 uses exception levels, usually written as EL0 through EL3.

## The levels you need here

### EL0
User-space applications.

### EL1
Normal kernel execution level. Linux kernel code typically runs here.

### EL2
Hypervisor level. Some systems boot into EL2 first and then drop into EL1. Linux may configure EL2 before continuing at EL1.

## Why `__cpu_setup` cares

Before `__cpu_setup` runs, the boot code has already passed through `init_kernel_el` in `head.S`. That code ensures the system is entering the correct execution path and prepares for EL1 execution.

That means `__cpu_setup` focuses mainly on EL1 register state:

- `CPACR_EL1`
- `MDSCR_EL1`
- `MAIR_EL1`
- `TCR_EL1`
- `TCR2_EL1`
- final `SCTLR_EL1` value returned in `x0`

## Important point

`__cpu_setup` is not deciding whether Linux runs at EL0 or EL1. That is already established. Instead, it prepares the EL1 memory-management environment.

## Boot reality

A board may start the CPU in EL2 or EL1 depending on firmware and platform setup. Linux normalizes that early. After that, `__cpu_setup` prepares the translation environment expected by the kernel.
