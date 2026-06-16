# ARM64 Overview

ARM64, also called AArch64, is the 64-bit execution state of the ARM architecture. In the Linux kernel on modern ARMv8 systems, the earliest boot code runs in AArch64 assembly before C code takes over.

## Why this matters for `__cpu_setup`

`__cpu_setup` runs very early, before the normal kernel runtime is available. At this stage:

- The kernel cannot rely on normal virtual memory yet.
- The MMU is still off when the function begins.
- The CPU is running with a small amount of carefully prepared early state.
- The code must program hardware directly through system registers.

## Three viewpoints you need

### CPU viewpoint
The CPU fetches and executes instructions. It has general-purpose registers like `x0` to `x30`, special registers like `sp`, and system registers like `SCTLR_EL1`, `TCR_EL1`, and `MAIR_EL1`.

### Memory viewpoint
Without the MMU, instruction fetches and data accesses happen using physical addressing behavior. With the MMU enabled, virtual addresses are translated through page tables, and memory attributes become important.

### Kernel viewpoint
The Linux kernel needs to move from a fragile early boot state into a fully mapped kernel address space. `__cpu_setup` is one of the last setup steps before that transition.

## What ARMv8 gives us

For this learning repo, assume ARMv8-A with these ideas in mind:

- Multiple exception levels
- A 64-bit register file
- System registers for memory management and privilege control
- Optional architecture extensions that may or may not be present on the running CPU

## A useful mental picture

Think of the early boot process like preparing a room before turning on the main power distribution:

- Page tables are built.
- Register policies are chosen.
- Translation settings are loaded.
- Then a final switch is flipped.

In this analogy, `__cpu_setup` is the preparation phase, not the final power-on switch itself.
