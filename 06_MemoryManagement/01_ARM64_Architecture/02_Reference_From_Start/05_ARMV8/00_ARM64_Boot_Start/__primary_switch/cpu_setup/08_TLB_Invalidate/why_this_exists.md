# Why This Exists (08_TLB_Invalidate)

## Design Rationale
- Inherited TLB state may encode old MAIR/TCR interpretation rules.
- Using stale entries with new control policy is architecturally unsafe.
- Global invalidate is cheap at boot compared to silent memory corruption.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
