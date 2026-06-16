# Why This Exists (13_IPS_and_PARange)

## Design Rationale
- IPS controls legal physical address size for stage-1 translation.
- Overstating IPS can generate invalid physical accesses.
- Understating IPS can make valid RAM unreachable.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
