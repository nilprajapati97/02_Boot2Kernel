# What The Code Says (02_Exception_Levels_and_Privilege)

## Section Focus
Privilege boundaries (EL3/EL2/EL1/EL0) and why __cpu_setup hardens EL1 defaults.

## Concrete Operations
- Kernel entry code ensures execution in EL1 context for this path.
- __cpu_setup disables user-visible debug/perf exposure via CPACR/MDSCR helpers.
- Caller later enables MMU with EL1-owned SCTLR policy.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
