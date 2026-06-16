# CPU/Register/Memory View (12_VA52_and_LPA2)

## Register Perspective
- TCR TxSZ fields interact with effective VA width.
- ID_AA64MMFR2_EL1 advertises support level for extended addressing.
- Compile-time guards must align with runtime probe decisions.

## Memory and Translation Perspective
- Changes virtual coverage and potentially descriptor layout assumptions.
- Impacts kernel/user split boundaries under high-VA configurations.
- Influences how top bits are treated during address checks.

## Debug Checklist
- If high VA kernel builds fault early, verify VA52 runtime gate.
- Do not enable VA52 solely from Kconfig without runtime support check.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
