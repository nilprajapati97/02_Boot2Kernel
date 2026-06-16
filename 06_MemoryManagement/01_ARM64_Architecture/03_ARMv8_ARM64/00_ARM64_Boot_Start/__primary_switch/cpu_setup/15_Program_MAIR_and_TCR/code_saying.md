# What The Code Says (15_Program_MAIR_and_TCR)

## Section Focus
Architectural commit point: writing MAIR_EL1 and TCR_EL1 from prepared scratch values.

## Concrete Operations
- msr mair_el1, mair commits attribute table used by descriptors.
- msr tcr_el1, tcr commits walk split, granule, and cache/share policy.
- Ordering is maintained so later enable uses coherent policy set.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
