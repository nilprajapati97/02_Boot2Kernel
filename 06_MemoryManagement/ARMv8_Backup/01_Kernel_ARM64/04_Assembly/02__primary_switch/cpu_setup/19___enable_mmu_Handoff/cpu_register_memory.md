# CPU/Register/Memory View (19___enable_mmu_Handoff)

## Register Perspective
- Input to caller: x0 contains MMU-on SCTLR template.
- Caller-owned writes: TTBR0_EL1, TTBR1_EL1, SCTLR_EL1, barrier sequence.
- Setup-owned state already committed: MAIR_EL1 and TCR_EL1.

## Memory and Translation Perspective
- After ISB, fetch and data accesses obey new translation regime.
- TLB repopulates under the new policy from live page tables.
- Control transfer must target mapped executable kernel VA.

## Debug Checklist
- If failure occurs exactly after SCTLR write, validate branch target mapping.
- If failure occurs only on one path, compare primary vs resume handoff ordering.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
