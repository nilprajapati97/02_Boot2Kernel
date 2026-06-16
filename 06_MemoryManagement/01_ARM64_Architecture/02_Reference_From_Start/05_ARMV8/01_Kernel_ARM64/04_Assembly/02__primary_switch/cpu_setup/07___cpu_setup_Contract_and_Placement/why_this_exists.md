# Why This Exists (07___cpu_setup_Contract_and_Placement)

## Design Rationale
- Executing from virtual-only text during MMU transitions can deadlock fetch.
- Contract clarity prevents callers from assuming TTBR or SCTLR side effects.
- Placement guarantees deterministic instruction fetch during translation churn.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
