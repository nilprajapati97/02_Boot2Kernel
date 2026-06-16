# Why This Exists (14_Hardware_AF_and_HAFT)

## Design Rationale
- Hardware AF can reduce software page-fault overhead.
- Blindly enabling unsupported AF mode can trap or misbehave.
- Consistent gating keeps behavior stable across heterogeneous CPUs.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
