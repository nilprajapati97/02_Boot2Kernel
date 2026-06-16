# Why This Exists (20_Secondary_CPU_and_Resume)

## Design Rationale
- Per-CPU control registers are not globally synchronized by default.
- Low-power retention can preserve unsafe or stale translation context.
- One setup implementation keeps all CPU entry paths aligned.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
