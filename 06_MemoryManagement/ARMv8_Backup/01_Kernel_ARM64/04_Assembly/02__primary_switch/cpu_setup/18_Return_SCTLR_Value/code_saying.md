# What The Code Says (18_Return_SCTLR_Value)

## Section Focus
Preparing and returning INIT_SCTLR_EL1_MMU_ON instead of writing SCTLR_EL1 directly.

## Concrete Operations
- Load INIT_SCTLR_EL1_MMU_ON constant into x0 near function end.
- Return with ret so caller controls exact activation point.
- Caller writes SCTLR_EL1 and executes required ISB sequence.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
