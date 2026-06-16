# 03 VMSA And Address Translation

The Virtual Memory System Architecture, or VMSA, defines how virtual addresses are translated, how page-table walks happen, how attributes are carried, and how TLBs cache translation results.

## The Basic Pipeline

When a load, store, or instruction fetch uses a virtual address, the core must determine:

- whether translation is enabled
- which translation base register is relevant
- what address size and granule are in effect
- how many table levels must be walked
- what memory type and permissions the final descriptor implies

Those decisions are not stored in one place. They come from the combined state of `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, page-table memory, and `MAIR_EL1`.

## Why `__cpu_setup` Matters To VMSA

`__cpu_setup` programs the control side of that pipeline:

- `MAIR_EL1` gives meaning to `AttrIndx`
- `TCR_EL1` defines the translation regime
- `TCR2_EL1` extends that regime on CPUs with newer features
- `SCTLR_EL1` is prepared for the caller to commit

Without those registers, page tables alone are not enough.

## TTBR0 Versus TTBR1

Linux uses the two halves of the EL1 stage-1 regime differently:

- `TTBR0_EL1` is used for the identity map during early bring-up and later for user-space side semantics
- `TTBR1_EL1` is used for the kernel half of the address space

That is why `T0SZ` and `T1SZ` are intentionally not symmetric in the Linux boot code.

## Why TLB State Matters Before The Switch

A TLB caches translation outcomes. If Linux changes translation-control state but stale TLB contents remain valid, the core can continue using obsolete mappings or obsolete interpretation rules. That is why `__cpu_setup` starts with a TLB invalidation step before the kernel installs its own final regime.