# What The Code Says (08_TLB_Invalidate)

## Section Focus
The opening tlbi vmalle1 and dsb nsh sequence and its architectural role.

## Concrete Operations
- Issue tlbi vmalle1 to invalidate EL1 stage-1 translation entries.
- Issue dsb nsh to wait for invalidate completion before proceeding.
- Continue with register programming only after stale walk state is gone.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
