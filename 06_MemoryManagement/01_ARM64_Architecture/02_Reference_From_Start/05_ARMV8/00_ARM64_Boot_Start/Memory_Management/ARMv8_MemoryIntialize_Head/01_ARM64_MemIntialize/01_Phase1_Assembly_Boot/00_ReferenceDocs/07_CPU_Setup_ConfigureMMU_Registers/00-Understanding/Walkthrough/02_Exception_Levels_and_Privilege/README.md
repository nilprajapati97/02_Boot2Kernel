# 02 Exception Levels And Privilege

ARMv8-A has multiple exception levels. For this topic, the important levels are EL2 and EL1.

## Why EL Matters Here

The Linux kernel normally wants to run at EL1. Some systems boot the kernel at EL2 first, usually because firmware or a boot environment leaves the CPU there. In that case Linux must drop or configure state so the normal kernel execution environment is coherent.

This is handled by `init_kernel_el` in `head.S` before `__cpu_setup` runs.

## Roles Of The Relevant Levels

- EL2 is the hypervisor level in the non-secure world.
- EL1 is the privileged operating-system level where Linux normally executes.
- EL0 is unprivileged software, that is, user space.

`__cpu_setup` runs after Linux has arranged the effective kernel execution context, so its job is focused on EL1 architectural state rather than exception-level transition policy.

## Why Some EL0-Visible Registers Are Reset Here

Even though the kernel is still in early boot, Linux already cares about what EL0 would eventually be allowed to touch. That is why the path resets controls like `PMUSERENR_EL0` and `AMUSERENR_EL0` and sets `MDSCR_EL1` to block EL0 Debug Communications Channel access. Firmware might have left those in an open or inconsistent state.

That is a hardware hygiene step. It prevents privilege leakage from inherited boot firmware state.

## Practical Takeaway

By the time `__cpu_setup` starts, Linux has already decided which execution level model it wants. `__cpu_setup` is not about negotiating privilege levels. It is about making the chosen EL1 environment architecturally ready for Linux's translation regime.