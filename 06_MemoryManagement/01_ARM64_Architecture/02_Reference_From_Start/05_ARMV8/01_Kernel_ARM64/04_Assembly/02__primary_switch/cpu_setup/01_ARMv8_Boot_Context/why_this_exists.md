# Why This Exists (01_ARMv8_Boot_Context)

## Design Rationale
- Boot firmware and kernel do not share identical MMU expectations.
- Kernel must normalize control state regardless of vendor firmware behavior.
- Reset/resume paths require repeatable EL1 setup semantics.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
