# CPU/Register/Memory View (21_Register_Atlas)

## Register Perspective
- Write set: CPACR_EL1, MDSCR_EL1, MAIR_EL1, TCR_EL1, optional TCR2_EL1.
- Probe set: MIDR_EL1 and ID_AA64MMFR* family fields.
- Caller set: TTBR0_EL1, TTBR1_EL1, SCTLR_EL1 final activation.

## Memory and Translation Perspective
- Register map determines translation, permissions, and observability surfaces.
- Wrong ownership assumptions lead to duplicate or missing writes.
- Atlas shortens debug loops when early boot has no console.

## Debug Checklist
- Use this section to map exception syndrome to likely register owner.
- When in doubt, verify owner before patching call-site code.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
