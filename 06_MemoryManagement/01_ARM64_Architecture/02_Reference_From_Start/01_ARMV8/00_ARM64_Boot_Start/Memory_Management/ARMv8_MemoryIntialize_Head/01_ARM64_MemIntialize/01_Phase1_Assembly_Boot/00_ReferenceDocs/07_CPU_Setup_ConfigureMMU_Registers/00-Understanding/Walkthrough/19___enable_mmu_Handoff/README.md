# 19 `__enable_mmu` Handoff

`__enable_mmu` is where the prepared control state becomes a live translation regime.

## Inputs

- `x0`: final `SCTLR_EL1` value returned by `__cpu_setup`
- `x1`: kernel table base value for `TTBR1_EL1`
- `x2`: idmap root table address for `TTBR0_EL1`

## Sequence

1. check granule support using `ID_AA64MMFR0_EL1`
2. convert physical addresses into TTBR format where required
3. write `TTBR0_EL1`
4. write `TTBR1_EL1`
5. call `set_sctlr_el1 x0`

## Why This Is The Commit Point

This is the first place where both the table base registers and the final `SCTLR_EL1` value are present together in the right order. Only here does the hardware have everything it needs to execute under the Linux-owned stage-1 regime.

## What `set_sctlr_el1` Adds

The helper writes `SCTLR_EL1`, performs the necessary instruction synchronization, invalidates the local I-cache to discard any stale speculative fetches, and issues the required barriers. That is part of making the transition architecturally clean.