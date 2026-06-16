# What The Code Says (21_Register_Atlas)

## Section Focus
Reference map of every register touched or consumed by setup and handoff flow.

## Concrete Operations
- Catalog write-side registers vs read/probe-only registers.
- Map register fields to decision points in sections 07-19.
- Document caller-owned registers outside __cpu_setup responsibility.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
