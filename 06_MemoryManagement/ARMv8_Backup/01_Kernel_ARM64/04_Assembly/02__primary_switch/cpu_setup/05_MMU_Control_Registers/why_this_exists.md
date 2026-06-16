# Why This Exists (05_MMU_Control_Registers)

## Design Rationale
- Separating build from activation avoids half-configured translation state.
- Callers own TTBR install timing, so setup must be reusable.
- Register ownership clarity simplifies resume and secondary-CPU paths.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
