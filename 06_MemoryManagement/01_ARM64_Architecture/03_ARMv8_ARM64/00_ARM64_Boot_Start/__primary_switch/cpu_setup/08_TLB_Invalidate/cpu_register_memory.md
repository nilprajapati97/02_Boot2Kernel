# CPU/Register/Memory View (08_TLB_Invalidate)

## Register Perspective
- No general-purpose register is architecturally updated by tlbi itself.
- Barrier completion order governs when following msr writes are safe.
- Subsequent TCR/MAIR writes rely on this clean baseline.

## Memory and Translation Perspective
- Forces fresh page walks after MMU enable using new policy.
- Prevents stale walk-cache entries from bypassing new configuration.
- Reduces cross-path variance between cold boot and resume.

## Debug Checklist
- Intermittent early faults often disappear when this sequence is restored.
- Do not replace vmalle1 with narrow invalidation in early boot code.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
