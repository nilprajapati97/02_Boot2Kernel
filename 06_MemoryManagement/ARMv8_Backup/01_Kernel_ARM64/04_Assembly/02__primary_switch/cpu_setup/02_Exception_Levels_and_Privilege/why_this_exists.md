# Why This Exists (02_Exception_Levels_and_Privilege)

## Design Rationale
- Privilege leakage from inherited firmware state is a security risk.
- Early lock-down prevents EL0 observability before policy code runs.
- Separation of setup and MMU enable keeps responsibility boundaries clear.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
