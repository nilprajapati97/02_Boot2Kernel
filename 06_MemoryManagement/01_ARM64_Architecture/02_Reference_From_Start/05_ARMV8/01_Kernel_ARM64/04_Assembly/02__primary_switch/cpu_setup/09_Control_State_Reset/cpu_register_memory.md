# CPU/Register/Memory View (09_Control_State_Reset)

## Register Perspective
- CPACR_EL1 is forced to deterministic baseline.
- MDSCR_EL1 TDCC bit is asserted for debug channel control.
- PMUSERENR/AMUSERENR paths are reset via helper macros.

## Memory and Translation Perspective
- Shrinks side-channel exposure before scheduler and mitigations complete.
- Reduces visibility of microarchitectural counters to EL0.
- Keeps early boot security state consistent across CPUs.

## Debug Checklist
- If EL0 unexpectedly reads PMU counters, inspect reset helper patching.
- If debug pathways remain open, confirm MDSCR write made it to hardware.

## Failure Signature to Watch
- If behavior differs between primary boot and resume, suspect missing reinitialization.
- If behavior differs by CPU model, suspect feature/errata probe gating.
