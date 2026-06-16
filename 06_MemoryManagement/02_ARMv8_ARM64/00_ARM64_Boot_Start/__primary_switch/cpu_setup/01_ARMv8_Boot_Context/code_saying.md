# What The Code Says (01_ARMv8_Boot_Context)

## Section Focus
ARMv8-A reset and early EL transitions that constrain how __cpu_setup must run.

## Concrete Operations
- CPU starts from firmware-defined state and enters kernel entry stubs.
- Kernel reaches EL1 with interrupts masked and minimal mapping assumptions.
- __cpu_setup executes before final kernel VA layout is live.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
