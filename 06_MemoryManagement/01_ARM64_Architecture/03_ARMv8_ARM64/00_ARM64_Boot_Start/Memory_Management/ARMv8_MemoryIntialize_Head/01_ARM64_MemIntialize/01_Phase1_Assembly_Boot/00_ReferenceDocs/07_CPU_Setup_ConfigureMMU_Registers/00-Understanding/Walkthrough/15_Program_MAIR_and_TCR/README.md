# 15 Program `MAIR_EL1` And `TCR_EL1`

Only after Linux has finished runtime adjustments does it execute:

```asm
msr mair_el1, mair
msr tcr_el1, tcr
```

## Why The Write Is Delayed Until Here

Linux intentionally computes the values first, then commits them after:

- errata scrubbing
- VA52 and LPA2 adjustments
- physical-address-size computation
- hardware AF and HAFT decisions

If Linux wrote `TCR_EL1` earlier, later fixes would either require another write or risk a short window where the live architectural state did not match the final intended policy.

## Hardware Meaning

At this moment the CPU's translation-control registers are programmed, but the regime is still not fully active because the table base registers are not yet loaded for the handoff and the final `SCTLR_EL1` enable write has not happened.

This is the difference between defining the regime and committing to execution under that regime.