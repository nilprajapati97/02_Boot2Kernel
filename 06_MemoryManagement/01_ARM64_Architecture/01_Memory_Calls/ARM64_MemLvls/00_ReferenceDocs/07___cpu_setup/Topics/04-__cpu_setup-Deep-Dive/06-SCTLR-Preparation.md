# SCTLR Preparation

The function ends by loading `x0` with `INIT_SCTLR_EL1_MMU_ON` and returning.

## What this means

The final stage of this function is not another feature probe or register write. It is preparation of the value that the caller will use to turn on the MMU and caches.

## Why return instead of writing directly

The Linux boot flow separates concerns:

- `__cpu_setup` prepares the control state
- `__enable_mmu` installs the translation bases and writes `SCTLR_EL1`

This separation makes the flow easier to reason about and ensures page-table base programming happens in the right place.

## Mental model

If `MAIR_EL1` and `TCR_EL1` describe the rules of the road, `INIT_SCTLR_EL1_MMU_ON` is the key that starts the engine.
