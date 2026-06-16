# CPU/Register/Memory View (00_Reading_Guide)

## Register Perspective
- No register is written by this section; it defines the reading model.
- Primary registers to track in later sections are MAIR_EL1, TCR_EL1, and SCTLR_EL1.
- Secondary controls include CPACR_EL1, MDSCR_EL1, and optional TCR2_EL1 bits.

## Memory and Translation Perspective
- Track when VA to PA translation is disabled, partially configured, and fully enabled.
- Track TLB state transitions around tlbi vmalle1 and barriers.
- Track ownership of TTBR0/TTBR1 writes between __cpu_setup and caller paths.

## Debug Checklist
- If logs stop before MMU enable, return to sections 07, 15, 18, and 19.
- If board-specific faults occur, compare feature probes from sections 11-17.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
