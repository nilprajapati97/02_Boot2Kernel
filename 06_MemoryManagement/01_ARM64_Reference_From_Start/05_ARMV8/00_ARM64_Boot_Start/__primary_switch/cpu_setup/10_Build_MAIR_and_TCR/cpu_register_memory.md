# CPU/Register/Memory View (10_Build_MAIR_and_TCR)

## Register Perspective
- Scratch aliases commonly map to x17=mair, x16=tcr, x15=tcr2.
- Constants define default granule, cacheability, shareability, and split.
- No system register write occurs until patching phases complete.

## Memory and Translation Perspective
- Default MAIR slots define how descriptor AttrIndx maps to memory type.
- Default TCR fields define walk cache/shareability behavior.
- Wrong defaults create deterministic but incorrect memory semantics.

## Debug Checklist
- Dump scratch values in simulator before msr commits when debugging.
- Check literal constants against configured page size and VA bits.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
