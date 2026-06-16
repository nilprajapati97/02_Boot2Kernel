# 09 Control State Reset

The next block resets inherited control state:

```asm
msr cpacr_el1, xzr
mov x1, MDSCR_EL1_TDCC
msr mdscr_el1, x1
reset_pmuserenr_el0 x1
reset_amuserenr_el0 x1
```

## Register-Level Meaning

- `CPACR_EL1` controls access policy for coprocessor-style functionality, including FP/SIMD exposure paths.
- `MDSCR_EL1` is a monitor debug system control register. Linux sets the `TDCC` bit to block EL0 Debug Communications Channel access.
- `PMUSERENR_EL0` controls user-level PMU access.
- `AMUSERENR_EL0` controls user-level AMU access.

## Why This Happens In Early Boot

Linux wants a deterministic and locked-down baseline. Firmware may leave these registers in values that are legal for firmware but wrong for a general-purpose OS kernel.

The key point is that `__cpu_setup` is not only about translation. It is also about removing inherited architectural state that could leak privilege or confuse later kernel setup.

## Failure Mode If Omitted

The system may boot, but EL0-visible debug or monitoring access could be unintentionally open, and later kernel code would be starting from an unknown policy baseline.