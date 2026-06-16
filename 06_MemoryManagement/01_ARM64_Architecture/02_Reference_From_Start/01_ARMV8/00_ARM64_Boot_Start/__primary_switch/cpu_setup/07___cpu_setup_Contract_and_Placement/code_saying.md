# What The Code Says (07___cpu_setup_Contract_and_Placement)

## Section Focus
Exact contract of __cpu_setup and why it must run from .idmap.text.

## Concrete Operations
- Function starts in identity-mapped text so control writes are fetch-safe.
- Inputs are unconstrained scratch registers and inherited control state.
- Outputs are programmed MAIR/TCR state and x0 SCTLR template.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
