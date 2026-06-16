# CPU/Register/Memory View (04_Page_Tables_and_Descriptors)

## Register Perspective
- TTBR0_EL1/TTBR1_EL1 roots are caller-owned and must match TCR split.
- TCR_EL1 TGx and TxSZ fields control level count and address coverage.
- MAIR_EL1 slot usage must align with descriptor AttrIndx values.

## Memory and Translation Perspective
- Descriptor + MAIR mismatch can map device memory as cacheable.
- Walk cache behavior depends on TCR IRGN/ORGN/SH fields.
- Access flag/dirty tracking behavior can vary with optional features.

## Debug Checklist
- If MMIO reads look stale, verify descriptor AttrIndx and MAIR slot value.
- If user/kernel split looks wrong, validate TTBR roots against TCR split.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
