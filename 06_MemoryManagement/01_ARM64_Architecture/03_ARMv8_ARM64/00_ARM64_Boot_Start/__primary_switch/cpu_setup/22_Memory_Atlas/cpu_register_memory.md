# CPU/Register/Memory View (22_Memory_Atlas)

## Register Perspective
- TCR split and MAIR slots are the main memory-behavior controls.
- TTBR roots define walk origin once caller commits them.
- SCTLR activation is the transition edge from setup to live translation.

## Memory and Translation Perspective
- Documents identity map lifetime and teardown assumptions.
- Explains why stale TLB state must be removed before activation.
- Highlights device vs normal memory attribute boundaries.

## Debug Checklist
- If branch target faults, check whether that VA is valid pre/post handoff.
- If MMIO behaves oddly, validate memory attribute mapping path.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
