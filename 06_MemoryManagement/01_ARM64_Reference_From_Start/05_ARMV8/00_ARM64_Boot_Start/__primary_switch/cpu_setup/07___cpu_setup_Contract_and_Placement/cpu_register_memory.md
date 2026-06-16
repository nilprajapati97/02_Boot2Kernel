# CPU/Register/Memory View (07___cpu_setup_Contract_and_Placement)

## Register Perspective
- x0 returns INIT_SCTLR_EL1_MMU_ON to caller.
- TTBR0_EL1 and TTBR1_EL1 are intentionally untouched here.
- MAIR_EL1 and TCR_EL1 are committed before return.

## Memory and Translation Perspective
- Identity map avoids dependency on not-yet-final VA layout.
- Stale translation state is purged at function start.
- Memory-type and walk policy are fixed before caller enables translation.

## Debug Checklist
- If code aborts inside setup, verify .idmap.text mapping integrity.
- If caller expects TTBR writes from setup, fix caller contract assumptions.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
