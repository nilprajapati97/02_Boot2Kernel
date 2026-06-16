# Why This Exists (12_VA52_and_LPA2)

## Design Rationale
- VA width controls level count and canonical address interpretation.
- Not all hardware and kernels support expanded VA features.
- Incorrect VA width programming causes immediate translation aborts.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
