# What The Code Says (23_Mermaid_Diagrams)

## Section Focus
Visual execution and dependency diagrams for setup, probing, and MMU handoff.

## Concrete Operations
- Show control-flow graph from entry to return and caller handoff.
- Show feature-gate graph for errata and optional extensions.
- Show barrier and register dependency ordering.

## Audit Anchors
- Confirm this section's operations against arch/arm64/kernel/head.S.
- Validate call path context in primary, secondary, and resume flows.
- Keep assumptions aligned with runtime feature probing behavior.

## Reviewer Questions
- Which operation in this section changes architectural state?
- Which operation is caller-owned and intentionally out of scope?
- What is the first observable failure if this section is wrong?
