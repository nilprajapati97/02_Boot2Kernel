# What The Code Says (22_Memory_Atlas)

## Section Focus
Memory-space view of idmap, kernel VA layout, and translation-state transitions.

## Concrete Operations
- Track execution from identity map to final kernel virtual mappings.
- Track when page-walk policy switches from inherited to kernel-defined.
- Track TTBR-root assumptions at each transition checkpoint.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
