# Why This Exists (18_Return_SCTLR_Value)

## Design Rationale
- TTBR install timing belongs to caller, not setup helper.
- Separated activation avoids enabling MMU before address roots are ready.
- Supports reuse across primary, secondary, and resume call paths.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
