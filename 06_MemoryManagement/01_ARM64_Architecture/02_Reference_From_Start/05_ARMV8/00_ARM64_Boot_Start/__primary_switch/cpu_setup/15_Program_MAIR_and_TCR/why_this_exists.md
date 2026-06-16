# Why This Exists (15_Program_MAIR_and_TCR)

## Design Rationale
- Committing both registers near each other minimizes inconsistent windows.
- Post-commit behavior must match descriptor assumptions prepared by caller.
- This is the last major setup point before MMU-on handoff.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
