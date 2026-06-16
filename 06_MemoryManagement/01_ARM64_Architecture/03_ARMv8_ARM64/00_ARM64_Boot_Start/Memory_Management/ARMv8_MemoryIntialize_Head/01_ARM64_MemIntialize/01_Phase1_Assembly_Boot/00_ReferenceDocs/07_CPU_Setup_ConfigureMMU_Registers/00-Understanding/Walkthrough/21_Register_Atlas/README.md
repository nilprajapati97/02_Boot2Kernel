# 21 Register Atlas

This chapter is a quick-reference map of the registers touched directly or indirectly by the path.

## `CPACR_EL1`

Access-control policy for certain architectural execution resources. Linux resets it to avoid inheriting surprising firmware policy.

## `MDSCR_EL1`

Monitor debug control state. Linux sets `TDCC` early to block EL0 Debug Communications Channel access.

## `PMUSERENR_EL0` And `AMUSERENR_EL0`

User-level enable controls for performance and activity monitoring facilities. Linux clears them early.

## `MAIR_EL1`

Maps descriptor `AttrIndx` values to concrete memory types.

## `TCR_EL1`

Defines stage-1 translation geometry and walk behavior, including `T0SZ`, `T1SZ`, granule, shareability, walk cacheability, TBI behavior, and `IPS`.

## `TCR2_EL1`

Extended translation control register used for newer features such as `HAFT` and permission-indirection enablement.

## `TTBR0_EL1` And `TTBR1_EL1`

Table-base registers for the two halves of the EL1 stage-1 address space.

## `SCTLR_EL1`

System control register that commits the live MMU and cache behavior.

## `ID_AA64MMFR0_EL1`

Feature-discovery source for `PARange` and granule information.

## `ID_AA64MMFR1_EL1`

Feature-discovery source for hardware AF and related capability levels.

## `ID_AA64MMFR3_EL1`

Feature-discovery source for newer extensions such as `S1PIE` and `TCRX`.

## `PIRE0_EL1` And `PIR_EL1`

Permission-indirection registers initialized only when the CPU advertises the corresponding feature.