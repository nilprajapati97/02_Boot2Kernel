# What The Code Says (11_Errata_Scrub)

## Section Focus
Clearing/patching TCR bits and control behavior for known CPU errata.

## Concrete Operations
- Read MIDR and related ID fields used by errata match logic.
- Apply tcr_clear_errata_bits style masks before final register writes.
- Select workaround paths with alternatives/feature patch framework.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
