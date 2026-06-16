# Why This Exists (09_Control_State_Reset)

## Design Rationale
- Firmware may leave permissive state that Linux must not trust.
- Least-privilege defaults are required before userspace can run.
- Consistent control reset avoids CPU-model-dependent behavior drift.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
