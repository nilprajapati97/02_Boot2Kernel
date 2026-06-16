# CPU/Register/Memory View (13_IPS_and_PARange)

## Register Perspective
- ID_AA64MMFR0_EL1.PARange is the hardware truth source.
- TCR_EL1.IPS must match or be below supported range.
- Kernel build options may constrain usable IPS choice.

## Memory and Translation Perspective
- Determines physical address bits used in descriptor output.
- Affects ability to map memory above 44/48-bit thresholds.
- Critical for large-memory server platforms.

## Debug Checklist
- If high-memory regions disappear, inspect IPS derivation path.
- If external abort appears on high PA access, check PARange decode.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
