# CPU/Register/Memory View (24_Interview_and_Debug_Notes)

## Register Perspective
- Prioritize SCTLR handoff, TTBR roots, MAIR, and TCR sanity checks.
- Then inspect CPACR/MDSCR/security resets for policy leakage.
- Finally inspect optional TCR2 and errata probe branches.

## Memory and Translation Perspective
- Correlate faulting VA with expected map phase (idmap vs final map).
- Correlate random faults with stale TLB or wrong attribute policy.
- Correlate CPU-specific failures with errata/feature gate branches.

## Debug Checklist
- Start with smallest reproducible boot path and one CPU core.
- Capture ESR/ELR/FAR and map to section ownership before patching.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
