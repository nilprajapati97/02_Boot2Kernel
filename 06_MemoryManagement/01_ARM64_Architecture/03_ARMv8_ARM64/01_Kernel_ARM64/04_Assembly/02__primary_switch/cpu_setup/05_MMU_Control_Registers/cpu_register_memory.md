# CPU/Register/Memory View (05_MMU_Control_Registers)

## Register Perspective
- Written: CPACR_EL1, MDSCR_EL1, MAIR_EL1, TCR_EL1, optional TCR2_EL1.
- Not written here: TTBR0_EL1, TTBR1_EL1, SCTLR_EL1.M transition.
- Computed output: x0 = INIT_SCTLR_EL1_MMU_ON.

## Memory and Translation Perspective
- TLB is invalidated before control-state programming proceeds.
- New MAIR/TCR semantics become active when caller enables MMU.
- Barrier ordering ensures architecture-visible state before handoff.

## Debug Checklist
- If caller writes SCTLR too early, expect synchronous fault loops.
- If TCR2 write traps, feature guard logic is missing or wrong.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
