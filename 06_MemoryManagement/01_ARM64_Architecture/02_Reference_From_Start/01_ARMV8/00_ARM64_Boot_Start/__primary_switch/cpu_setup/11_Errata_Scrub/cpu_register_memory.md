# CPU/Register/Memory View (11_Errata_Scrub)

## Register Perspective
- MIDR_EL1 identifies implementer, part number, variant, and revision.
- TCR scratch value is sanitized for affected cores.
- Patched instruction streams may alter helper macro behavior.

## Memory and Translation Perspective
- Prevents subtle walk or permission failures on affected silicon.
- Improves correctness under stress and resume transitions.
- Avoids architecture-compliant but implementation-broken field combinations.

## Debug Checklist
- If bug reproduces only on one stepping, suspect missing errata match.
- Validate MIDR decode path before changing TCR masks.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
