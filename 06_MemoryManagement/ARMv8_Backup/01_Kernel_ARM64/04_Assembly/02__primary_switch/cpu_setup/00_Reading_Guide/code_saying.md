# What The Code Says (00_Reading_Guide)

## Section Focus
How to read this vault from architecture context to line-by-line __cpu_setup analysis.

## Concrete Operations
- Start with 01-06 for architecture context before reading assembly details.
- Use 07-19 in order to follow the exact __cpu_setup data path.
- Use 20-24 as lookup tables when debugging board-specific failures.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
