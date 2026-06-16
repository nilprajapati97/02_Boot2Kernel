# CPU/Register/Memory View (15_Program_MAIR_and_TCR)

## Register Perspective
- MAIR_EL1 values map AttrIndx to device/normal memory semantics.
- TCR_EL1 controls TTBR0/TTBR1 interpretation and walk behavior.
- No SCTLR write here; activation is still caller-owned.

## Memory and Translation Perspective
- All subsequent page walks use this MAIR/TCR policy.
- Incorrect commit ordering can produce hard-to-reproduce transient faults.
- Barrier discipline around caller enable finalizes visibility.

## Debug Checklist
- If only specific memory types fail, inspect MAIR slot encodings first.
- If address split is wrong, inspect TCR TxSZ and TGx fields.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
