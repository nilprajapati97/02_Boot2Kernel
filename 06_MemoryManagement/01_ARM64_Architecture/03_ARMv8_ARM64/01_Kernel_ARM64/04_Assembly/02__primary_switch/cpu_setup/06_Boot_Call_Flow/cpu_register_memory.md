# CPU/Register/Memory View (06_Boot_Call_Flow)

## Register Perspective
- Callers consume returned SCTLR value and own the final write.
- Callers also own TTBR programming and final branch into mapped kernel text.
- __cpu_setup remains responsible for MAIR/TCR and control reset baseline.

## Memory and Translation Perspective
- Primary and secondary cores must agree on walk/cache policy.
- Resume path must not trust retained TLB state across low-power transitions.
- Flow sequencing guarantees no execution from unmapped text at handoff.

## Debug Checklist
- If only secondary cores fail, compare call-flow parity with primary path.
- Resume-only bugs often indicate stale retention assumptions.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
