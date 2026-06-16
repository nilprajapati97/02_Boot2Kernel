# Why This Exists (00_Reading_Guide)

## Design Rationale
- A guided order prevents mixing architectural assumptions with implementation details.
- The boot path is fragile: one wrong assumption about TCR/MAIR can break early boot silently.
- This section minimizes cognitive load before deep register-level analysis.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
