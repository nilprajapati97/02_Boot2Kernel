# CPU/Register/Memory View (17_TCR2_Write_Guard)

## Register Perspective
- TCR2 scratch starts zero and accumulates optional feature bits.
- Capability probe uses ID register fields and alternatives framework.
- Final msr REG_TCR2_EL1 is conditional and side-effect free when skipped.

## Memory and Translation Perspective
- Avoids boot-time trap that would block any memory setup progress.
- Ensures optional features do not compromise baseline correctness.
- Keeps old-core path deterministic and minimal.

## Debug Checklist
- If early exception points near TCR2, confirm guard path execution.
- Disassemble final image to ensure alternatives patched expected branch.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
