# System Control Register

`SCTLR_EL1` is the register that contains the high-level control bits for EL1 execution.

## Why it matters here

The returned value from `__cpu_setup` is a prepared `SCTLR_EL1` constant called `INIT_SCTLR_EL1_MMU_ON`.

That means the function is effectively saying:

"Here is the exact control value the next stage should write when it is ready to turn the MMU and caches on."

## Important fields conceptually involved

- MMU enable
- data cache enable
- instruction cache enable
- alignment behavior
- several architectural hardening and control bits required by Linux policy

## Critical distinction

`__cpu_setup` does not do `msr sctlr_el1, x0` itself.

That write happens later through `set_sctlr_el1` in the MMU enable path.

## What would go wrong if this order were reversed

If the kernel enabled `SCTLR_EL1.M` before programming compatible `MAIR_EL1`, `TCR_EL1`, and page-table bases, translation could start with inconsistent state and the CPU could fault or execute unpredictably.
