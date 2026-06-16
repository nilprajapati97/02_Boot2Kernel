# CPU/Register/Memory View (20_Secondary_CPU_and_Resume)

## Register Perspective
- Each CPU must program its own MAIR_EL1/TCR_EL1 instance.
- Resume flow reasserts CPACR/MDSCR defaults and optional feature bits.
- Caller-side TTBR/SCTLR ordering still applies per core.

## Memory and Translation Perspective
- Per-core TLBs require local invalidation and rewarm.
- Resume correctness depends on flushing stale translation assumptions.
- Cross-core consistency needs identical policy application.

## Debug Checklist
- If only resumed cores fail, compare retained state handling.
- If only secondaries fail, inspect secondary entry call ordering.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
