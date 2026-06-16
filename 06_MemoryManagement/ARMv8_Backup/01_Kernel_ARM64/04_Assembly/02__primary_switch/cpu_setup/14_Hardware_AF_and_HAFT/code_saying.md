# What The Code Says (14_Hardware_AF_and_HAFT)

## Section Focus
Enabling hardware Access Flag behavior when CPU capability allows it.

## Concrete Operations
- Probe feature level for hardware access flag updates.
- Set corresponding TCR/TCR2 bits (for HAFT-capable designs).
- Leave software AF path active on unsupported systems.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
