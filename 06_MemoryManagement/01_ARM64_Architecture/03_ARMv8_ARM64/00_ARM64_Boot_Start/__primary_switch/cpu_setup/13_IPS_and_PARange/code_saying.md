# What The Code Says (13_IPS_and_PARange)

## Section Focus
Selecting TCR IPS field from physical address range capability.

## Concrete Operations
- Read ID_AA64MMFR0_EL1 PARange field.
- Translate PARange encoding to TCR_EL1 IPS bits.
- Patch tcr scratch value with selected IPS before commit.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
