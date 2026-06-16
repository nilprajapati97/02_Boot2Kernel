# Why This Exists (03_VMSA_and_Address_Translation)

## Design Rationale
- Translation behavior must be deterministic before enabling M bit.
- Descriptor interpretation depends on MAIR/TCR coherence.
- Deferring SCTLR write prevents activating MMU before TTBR roots are ready.

## Security and Reliability Consequences
- Early-boot correctness depends on deterministic control-state normalization.
- Section-specific guardrails reduce architecture-dependent regressions.
- Isolated responsibilities make failures easier to localize and fix.

## Practical Rule
- Keep this section focused on one ownership boundary and one failure mode family.
