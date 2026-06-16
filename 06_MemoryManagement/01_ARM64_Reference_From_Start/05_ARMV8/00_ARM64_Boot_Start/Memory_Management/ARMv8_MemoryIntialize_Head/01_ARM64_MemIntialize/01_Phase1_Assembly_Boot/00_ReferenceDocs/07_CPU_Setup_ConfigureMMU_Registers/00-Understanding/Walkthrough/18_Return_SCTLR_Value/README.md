# 18 Return Final `SCTLR_EL1` Value

The routine ends by doing this:

```asm
mov_q x0, INIT_SCTLR_EL1_MMU_ON
ret
```

## Why Return Instead Of Write

This is one of the most important design choices in the whole path.

Linux returns the desired `SCTLR_EL1` value to the caller instead of writing it immediately because the commit point must happen only after:

- `TTBR0_EL1` is loaded
- `TTBR1_EL1` is loaded
- granule support has been verified
- the caller is ready to execute the transition under the identity-mapped code path

## What `INIT_SCTLR_EL1_MMU_ON` Represents

It is the boot-time policy bundle for major EL1 execution controls, including MMU enable, cache enable, alignment checks, and selected behavior/hardening bits Linux wants when the regime becomes live.

## Key Takeaway

`__cpu_setup` does not turn the MMU on. It returns the value that `__enable_mmu` will later commit at the correct sequencing point.