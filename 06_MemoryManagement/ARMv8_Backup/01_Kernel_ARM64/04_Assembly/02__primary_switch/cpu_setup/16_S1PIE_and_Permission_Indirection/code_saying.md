# What The Code Says (16_S1PIE_and_Permission_Indirection)

## Section Focus
Conditional setup for stage-1 permission indirection extensions.

## Concrete Operations
- Probe ID fields indicating S1PIE/permission indirection support.
- Set tcr2 scratch bits for permission indirection when supported.
- Preserve zero/default path on unsupported implementations.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
