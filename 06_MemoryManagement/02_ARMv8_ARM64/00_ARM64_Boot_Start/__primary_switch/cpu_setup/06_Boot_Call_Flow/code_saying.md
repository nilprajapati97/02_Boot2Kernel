# What The Code Says (06_Boot_Call_Flow)

## Section Focus
Who calls __cpu_setup and where control transfers before and after it.

## Concrete Operations
- Primary path calls __cpu_setup before __enable_mmu and primary switch.
- Secondary path calls __cpu_setup before joining common kernel entry.
- Resume path calls __cpu_setup to re-establish architectural defaults.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
