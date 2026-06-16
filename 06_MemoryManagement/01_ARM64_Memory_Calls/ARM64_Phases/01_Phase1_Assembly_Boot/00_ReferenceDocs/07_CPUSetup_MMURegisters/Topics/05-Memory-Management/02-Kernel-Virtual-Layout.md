# Kernel Virtual Layout

The ARM64 Linux kernel uses a structured virtual-address layout that depends on constants such as `VA_BITS` and `VA_BITS_MIN`.

## Why this matters to `__cpu_setup`

The function uses:

- `IDMAP_VA_BITS` for the idmap side
- `VA_BITS_MIN` for the kernel side
- optional VA52 handling for wider address support

These are not random constants. They define how large the translation regions are expected to be.

## Beginner intuition

`T0SZ` and `T1SZ` tell the MMU how much of the lower and upper virtual-address space should be considered part of the translation regime.

That means address-space layout policy is already being decided before the MMU is turned on.
