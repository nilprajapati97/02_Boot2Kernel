# Why This Exists (11_Errata_Scrub)

## Design Rationale
- Some cores mis-handle specific TCR combinations without mitigation.
- Errata workarounds must execute before MMU is enabled.
- Centralized scrub prevents per-callsite workaround drift.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
