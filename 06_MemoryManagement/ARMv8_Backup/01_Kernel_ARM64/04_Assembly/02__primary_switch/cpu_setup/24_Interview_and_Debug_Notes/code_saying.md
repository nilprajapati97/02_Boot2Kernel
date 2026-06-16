# What The Code Says (24_Interview_and_Debug_Notes)

## Section Focus
Operational troubleshooting playbook for interviews, bringup, and regression triage.

## Concrete Operations
- Map common failure signatures to likely section and register owner.
- Provide minimal debug checklist for no-console early boot failures.
- Document repeatable questions to validate setup assumptions quickly.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
