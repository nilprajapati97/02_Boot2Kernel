# 05 MMU Control Registers

These are the main registers you need in your head before reading `__cpu_setup`.

## `MAIR_EL1`

`MAIR_EL1` defines the meaning of memory attribute indices used by page descriptors. Without it, `AttrIndx` fields in the tables are just small numbers with no concrete memory behavior attached.

## `TCR_EL1`

`TCR_EL1` defines the geometry and behavior of stage-1 translation. It controls the size of the virtual-address regions for `TTBR0_EL1` and `TTBR1_EL1`, the granule size, page-walk cacheability, page-walk shareability, top-byte-ignore behavior, physical-address size, and several optional controls.

## `TCR2_EL1`

`TCR2_EL1` is an extension point for newer translation controls that do not fit into the classic `TCR_EL1` space. Linux only writes it if the current CPU advertises that the extended register exists.

## `TTBR0_EL1` And `TTBR1_EL1`

These hold the base addresses of the translation tables. During the final handoff, `__enable_mmu` loads them before writing `SCTLR_EL1`.

## `SCTLR_EL1`

`SCTLR_EL1` contains the major execution controls, including the bit that enables stage-1 translation and the cache-enable bits. It is the commit point for the new regime.

## Feature Discovery Registers

The ID registers are how Linux learns what the current CPU actually implements.

- `ID_AA64MMFR0_EL1` contributes physical-address and granule information
- `ID_AA64MMFR1_EL1` contributes hardware AF and related capability information
- `ID_AA64MMFR3_EL1` contributes newer features such as stage-1 permission indirection and `TCR2_EL1` availability

The theme of `__cpu_setup` is that Linux combines build-time policy with runtime-discovered capability.