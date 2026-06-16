# CPU/Register/Memory View (02_Exception_Levels_and_Privilege)

## Register Perspective
- CPACR_EL1 controls EL0/EL1 FP and SIMD access model.
- MDSCR_EL1_TDCC blocks debug communication channel visibility from EL0.
- PMU/AMU user access reset helpers force least-privilege defaults.

## Memory and Translation Perspective
- Privilege controls alter who can observe memory timing/telemetry surfaces.
- Early privilege reset reduces side-channel surface before scheduler starts.
- No user mapping is trusted until TTBR and SCTLR handoff completes.

## Debug Checklist
- Unexpected EL0 PMU visibility usually means reset helper path was skipped.
- Validate CPACR/MDSCR values on secondary CPU bringup as well.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
