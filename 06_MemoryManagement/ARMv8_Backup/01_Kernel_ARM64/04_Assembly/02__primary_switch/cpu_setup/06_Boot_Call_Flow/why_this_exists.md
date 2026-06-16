# Why This Exists (06_Boot_Call_Flow)

## Design Rationale
- All entry paths need identical EL1 translation baseline.
- Duplication across call sites would drift and introduce subtle regressions.
- Single setup path reduces CPU-model conditional logic spread.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
