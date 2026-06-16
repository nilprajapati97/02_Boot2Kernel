# What The Code Says (05_MMU_Control_Registers)

## Section Focus
Control registers whose lifecycle is split between __cpu_setup and its callers.

## Concrete Operations
- Reset control exposure registers (CPACR, MDSCR, PMU/AMU helpers).
- Build target MAIR/TCR/TCR2 values in scratch registers.
- Return prepared SCTLR value rather than writing it in-place.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
