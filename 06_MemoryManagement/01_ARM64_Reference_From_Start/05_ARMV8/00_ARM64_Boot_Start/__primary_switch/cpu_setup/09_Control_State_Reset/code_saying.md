# What The Code Says (09_Control_State_Reset)

## Section Focus
Resetting CPACR, MDSCR, PMU, and AMU access surfaces before MMU handoff.

## Concrete Operations
- msr cpacr_el1, xzr removes permissive inherited coprocessor policy.
- msr mdscr_el1, MDSCR_EL1_TDCC clamps debug communications access.
- reset_pmuserenr_el0 and reset_amuserenr_el0 clear EL0 telemetry paths.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
