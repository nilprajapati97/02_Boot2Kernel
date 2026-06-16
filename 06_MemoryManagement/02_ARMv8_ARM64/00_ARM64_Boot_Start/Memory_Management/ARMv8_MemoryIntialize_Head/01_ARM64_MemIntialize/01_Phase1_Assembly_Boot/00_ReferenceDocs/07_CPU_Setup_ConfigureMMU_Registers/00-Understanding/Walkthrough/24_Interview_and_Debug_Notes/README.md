# 24 Interview And Debug Notes

This chapter compresses the full walkthrough into fast recall points.

## One-Sentence Senior Summary

`__cpu_setup` is the arm64 kernel's early EL1 translation-setup routine: it sanitizes inherited architectural state, builds and programs the translation-control registers that define Linux's stage-1 regime, conditionally adapts that regime to the current CPU's features, and returns the final `SCTLR_EL1` value for `__enable_mmu` to commit later.

## Best Interview Order

1. State the contract: prepare EL1 state, do not enable the MMU directly.
2. Explain why it runs in `.idmap.text`.
3. Explain the split with `__enable_mmu`.
4. Walk the blocks in order.
5. End with secondary CPU and resume reuse as proof of the function's true role.

## Common Debug Questions

### Why invalidate the TLB first?

Because stale translations are unsafe when Linux is about to change the translation regime.

### Why not write `SCTLR_EL1` inside `__cpu_setup`?

Because the table bases are loaded later in `__enable_mmu`, and the enable point must happen only after those writes.

### Why is `T1SZ` conservative first?

Because one kernel image may boot on CPUs with different virtual-address capabilities.

## Bring-Up Failure Clues

- early hang before `start_kernel`: suspect table-base loading, granule support, or `SCTLR_EL1` commit sequencing
- strange faults despite apparently correct tables: suspect `TCR_EL1`, `MAIR_EL1`, or stale TLB state
- behavior that differs by CPU model: suspect ID-register-gated features or errata scrubbing
- resume-only failures: verify that the resume path re-established the same architectural baseline before re-enabling translation