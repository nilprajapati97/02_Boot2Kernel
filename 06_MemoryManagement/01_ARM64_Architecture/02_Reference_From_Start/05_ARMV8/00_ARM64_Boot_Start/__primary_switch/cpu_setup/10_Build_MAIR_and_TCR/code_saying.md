# What The Code Says (10_Build_MAIR_and_TCR)

## Section Focus
Building default MAIR/TCR bitfields in scratch registers before feature patches.

## Concrete Operations
- Load MAIR_EL1_SET literal into mair scratch register.
- Load INIT_TCR_EL1_FLAGS literal into tcr scratch register.
- Initialize tcr2 scratch register to zero before optional feature ORing.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
