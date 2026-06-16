# Why This Exists (16_S1PIE_and_Permission_Indirection)

## Design Rationale
- Permission indirection is optional and must be feature-gated.
- Enabling unsupported bits can trap before kernel is fully live.
- Gated enable lets one kernel image run across mixed silicon.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
