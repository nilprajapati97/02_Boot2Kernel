# CPU/Register/Memory View (01_ARMv8_Boot_Context)

## Register Perspective
- PSTATE.DAIF masking state governs interruptibility during early EL1 work.
- SCTLR_EL1 inherited state is unknown at entry and must be normalized later.
- VBAR_EL1 and SP selection are entry prerequisites around, not inside, __cpu_setup.

## Memory and Translation Perspective
- Boot firmware mapping assumptions are not guaranteed to match kernel policy.
- Identity mapping protects instruction fetch while MMU policy is re-established.
- Translation safety depends on delaying MMU-on until TTBR roots and policy align.

## Debug Checklist
- If boot dies before __cpu_setup, validate EL transition and vector base setup.
- If boot dies at MMU handoff, validate sequencing between TTBR writes and SCTLR write.

## Failure Signature to Watch
- EL mismatch faults at entry usually indicate firmware handoff contract breaks.
- Early synchronous aborts at handoff usually indicate translation-policy mismatch.
