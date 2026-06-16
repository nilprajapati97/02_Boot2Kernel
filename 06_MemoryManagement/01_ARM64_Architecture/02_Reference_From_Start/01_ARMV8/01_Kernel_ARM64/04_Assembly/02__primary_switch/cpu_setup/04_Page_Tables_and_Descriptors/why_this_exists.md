# Why This Exists (04_Page_Tables_and_Descriptors)

## Design Rationale
- Kernel page descriptors are prebuilt but interpreted at enable time.
- Descriptor correctness is necessary but insufficient without matching MAIR/TCR.
- This section links software descriptor bits to hardware walker behavior.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
