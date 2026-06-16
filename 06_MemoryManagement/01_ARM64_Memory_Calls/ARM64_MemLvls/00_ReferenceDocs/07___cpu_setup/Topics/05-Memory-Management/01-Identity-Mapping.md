# Identity Mapping

An identity map is an early boot mapping where the active virtual address resolves in a way that keeps execution continuous during the transition to MMU-on execution.

## Why the kernel needs it

When the MMU is off, the CPU is not using the normal kernel virtual-address view. The kernel therefore prepares an early mapping that can bridge the transition.

## Relationship to `__cpu_setup`

`__cpu_setup` does not construct the identity map, but it depends on that preparation already being complete because the next stage is about to enable translation using those tables.

## Key concept

`TCR_T0SZ(IDMAP_VA_BITS)` in `__cpu_setup` shows that the identity-map side of translation is part of the policy being configured.
