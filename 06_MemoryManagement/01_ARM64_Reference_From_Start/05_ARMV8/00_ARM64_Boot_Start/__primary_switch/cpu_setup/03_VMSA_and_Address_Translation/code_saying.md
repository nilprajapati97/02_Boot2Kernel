# What The Code Says (03_VMSA_and_Address_Translation)

## Section Focus
ARM64 stage-1 translation model that __cpu_setup prepares but does not fully activate.

## Concrete Operations
- Prepare MAIR_EL1 attribute table used by page descriptors.
- Prepare TCR_EL1 sizing, granule, shareability, and cacheability rules.
- Return SCTLR_EL1 MMU-on value to caller for final activation.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
