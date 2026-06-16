# CPU/Register/Memory View (03_VMSA_and_Address_Translation)

## Register Perspective
- MAIR_EL1 maps AttrIndx values to memory type encodings.
- TCR_EL1 defines split and walk behavior for TTBR0 and TTBR1 regions.
- SCTLR_EL1.M is only toggled by caller after setup returns.

## Memory and Translation Perspective
- Page walks consume MAIR and TCR semantics immediately after MMU enable.
- Wrong TCR sizing can alias regions or fault valid kernel addresses.
- Translation cache must be clean before new walk rules apply.

## Debug Checklist
- Early translation fault at enable point often means TCR/TTBR mismatch.
- Cacheability anomalies often point to MAIR encoding mistakes.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
