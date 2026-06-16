# CPU/Register/Memory View (14_Hardware_AF_and_HAFT)

## Register Perspective
- ID registers define hardware AF support level.
- TCR2_EL1 HAFT-related bits are only valid on supporting cores.
- Fallback retains software-managed AF semantics.

## Memory and Translation Perspective
- Accessed-bit updates influence reclaim and aging decisions.
- Hardware-updated AF reduces fault churn during memory pressure.
- Mismatched AF policy can distort memory management heuristics.

## Debug Checklist
- If AF faults spike unexpectedly, verify capability gate branch.
- Mixed-capability systems require conservative global policy.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
