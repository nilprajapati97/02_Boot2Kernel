# ARM64 Register Quick Reference

## `CPACR_EL1`
Used early to reset access-control state.

## `MDSCR_EL1`
Used early to control debug-related behavior.

## `MAIR_EL1`
Defines memory-attribute encodings used by page-table entries.

## `TCR_EL1`
Defines address-space sizing, granule size, table-walk cacheability, shareability, IPS, and optional translation behavior.

## `TCR2_EL1`
Defines newer extension controls such as PIE and HAFT when supported.

## `SCTLR_EL1`
Final high-level control register used to activate the MMU and caches.

## `ID_AA64MMFR1_EL1`
Feature-identification register used for HAFDBS-related checks.

## `ID_AA64MMFR3_EL1`
Feature-identification register used for PIE and TCR2 capability checks.

## `TTBR0_EL1`
Translation base for the lower address-space side in the early path.

## `TTBR1_EL1`
Translation base for the kernel high address-space side.
