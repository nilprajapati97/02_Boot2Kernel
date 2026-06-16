# CPU/Register/Memory View (23_Mermaid_Diagrams)

## Register Perspective
- Diagram nodes should include MAIR/TCR/TCR2 writes and SCTLR handoff.
- Probe nodes should include MIDR and ID register decisions.
- Caller nodes should include TTBR install and ISB boundary.

## Memory and Translation Perspective
- State-diagram should mark TLB clean point and MMU-on edge.
- Address-space diagram should mark idmap and kernel-map transition.
- Sequence diagram should mark where faults are most likely to occur.

## Debug Checklist
- Keep diagrams in sync with actual head.S line ordering.
- If flow and code diverge, trust code and update diagram immediately.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
