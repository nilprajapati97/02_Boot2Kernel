# What The Code Says (20_Secondary_CPU_and_Resume)

## Section Focus
How setup logic is reused for secondary core bringup and suspend/resume recovery.

## Concrete Operations
- Secondary entry invokes __cpu_setup before joining common kernel path.
- Resume path replays setup to discard stale retained control state.
- Both paths consume same x0 SCTLR handoff contract as primary boot.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
