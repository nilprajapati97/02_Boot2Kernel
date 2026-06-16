# What The Code Says (04_Page_Tables_and_Descriptors)

## Section Focus
Descriptor fields consumed by the MAIR/TCR policy configured in __cpu_setup.

## Concrete Operations
- Descriptor AttrIndx selects MAIR slot programmed in setup.
- Descriptor shareability interacts with TCR shareability defaults.
- Descriptor UXN/PXN and AP bits are meaningful only after coherent setup.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
