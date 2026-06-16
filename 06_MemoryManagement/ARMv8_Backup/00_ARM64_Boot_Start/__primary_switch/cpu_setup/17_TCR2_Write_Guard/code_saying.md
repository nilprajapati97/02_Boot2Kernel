# What The Code Says (17_TCR2_Write_Guard)

## Section Focus
Guarding optional TCR2_EL1 write so older CPUs never execute unsupported system writes.

## Concrete Operations
- Evaluate runtime capability indicating TCR2 presence.
- Branch around msr REG_TCR2_EL1 on unsupported hardware.
- Write TCR2 only after all feature bits are finalized.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
