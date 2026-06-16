# Why This Exists (17_TCR2_Write_Guard)

## Design Rationale
- Unconditional TCR2 write can raise undefined instruction exception.
- Guarded write keeps one binary portable across ARMv8 generations.
- Feature probing and write are intentionally adjacent for clarity.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
