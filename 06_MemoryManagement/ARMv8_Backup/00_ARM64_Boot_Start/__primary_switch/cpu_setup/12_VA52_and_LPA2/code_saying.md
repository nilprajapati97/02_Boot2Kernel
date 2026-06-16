# What The Code Says (12_VA52_and_LPA2)

## Section Focus
Conditional handling of 52-bit virtual addressing and LPA2-related controls.

## Concrete Operations
- Probe ID_AA64MMFR2_EL1 and build-time config for VA52 capability.
- Adjust TCR sizing/format assumptions when VA52 path is enabled.
- Keep fallback path for systems restricted to 48-bit VA.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
