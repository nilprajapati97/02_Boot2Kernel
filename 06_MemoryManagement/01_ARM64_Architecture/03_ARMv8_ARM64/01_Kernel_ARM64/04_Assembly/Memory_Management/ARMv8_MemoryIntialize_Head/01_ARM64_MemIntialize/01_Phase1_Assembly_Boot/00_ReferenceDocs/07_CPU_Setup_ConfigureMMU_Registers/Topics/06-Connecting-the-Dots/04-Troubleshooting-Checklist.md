# Troubleshooting Checklist

Use this checklist if you later debug early MMU bring-up on ARM64.

## Questions to ask

1. Were the idmap and kernel page tables created correctly?
2. Does the configured granule size match what the CPU supports?
3. Was `MAIR_EL1` programmed before enabling the MMU?
4. Was `TCR_EL1` programmed with the expected `T0SZ`, `T1SZ`, granule, shareability, and IPS fields?
5. Was `SCTLR_EL1` written only after the table bases were ready?
6. Were the required barriers executed?
7. Did a runtime feature branch set bits the CPU does not actually support?
8. Is a CPU erratum workaround required for this core?

## Debugging attitude

Early boot failures are often caused by one of three classes of problems:

- bad tables
- bad register policy
- wrong ordering

`__cpu_setup` sits squarely in the second class and partially in the third.
