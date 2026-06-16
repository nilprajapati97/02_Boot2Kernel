# Why This Exists (10_Build_MAIR_and_TCR)

## Design Rationale
- Building in GPRs allows conditional patching before architectural commit.
- Single commit point reduces partially-programmed register exposure.
- Facilitates errata and feature gating with simple bit operations.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
