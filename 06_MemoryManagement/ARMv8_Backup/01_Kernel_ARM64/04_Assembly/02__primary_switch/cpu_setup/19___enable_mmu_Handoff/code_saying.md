# What The Code Says (19___enable_mmu_Handoff)

## Section Focus
Precise handoff from __cpu_setup output state to __enable_mmu activation path.

## Concrete Operations
- Caller installs TTBR roots expected by prepared TCR split.
- Caller writes returned x0 value into SCTLR_EL1.
- Caller executes ISB and branches into fully translated kernel path.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
