# CPU/Register/Memory View (16_S1PIE_and_Permission_Indirection)

## Register Perspective
- ID_AA64MMFR3_EL1-like capability fields drive feature decision.
- TCR2_EL1 receives PIE-related bit updates on supported cores.
- Permission semantics then interact with page descriptor encodings.

## Memory and Translation Perspective
- Alters interpretation pipeline for access permission checks.
- Potentially reduces overhead for advanced permission models.
- Must remain coherent with page-table format assumptions.

## Debug Checklist
- Permission faults after enabling PIE suggest descriptor mismatch.
- Trap on TCR2 write indicates missing guard branch.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
