# CPU/Register/Memory View (18_Return_SCTLR_Value)

## Register Perspective
- x0 is the ABI return channel carrying SCTLR template value.
- SCTLR_EL1 itself is untouched inside __cpu_setup.
- Callers may mask/augment bits before final write if policy requires.

## Memory and Translation Perspective
- MMU remains effectively off or transitional until caller commits SCTLR.
- Avoids executing from unintended mappings in mid-setup state.
- Keeps final transition serialized with caller-specific barriers.

## Debug Checklist
- If setup returns but MMU enable fails, inspect caller write/ISB path.
- Do not assume returned value was already committed to SCTLR.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
