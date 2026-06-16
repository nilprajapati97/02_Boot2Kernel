# Code Walkthrough

This document walks through `__cpu_setup` block by block.

## Complete intent of the function

Prepare the CPU so that the following stage can safely load translation bases and enable the MMU at EL1.

## Block 1: clear local translation cache state

```asm
    tlbi    vmalle1
    dsb     nsh
```

Meaning:
- invalidate local EL1 stage-1 TLB entries
- wait for the invalidate to complete

Why:
- avoid stale translations when the new translation policy is committed

## Block 2: reset selected privileged-access registers

```asm
    msr     cpacr_el1, xzr
    mov     x1, MDSCR_EL1_TDCC
    msr     mdscr_el1, x1
    reset_pmuserenr_el0 x1
    reset_amuserenr_el0 x1
```

Meaning:
- clear `CPACR_EL1`
- reset `MDSCR_EL1` with the TDCC policy
- disable EL0 PMU access if PMU exists
- disable EL0 AMU access if AMU exists

Why:
- start from a clean, controlled privilege state

## Block 3: build the default memory and translation policy

```asm
    mair    .req x17
    tcr     .req x16
    tcr2    .req x15
    mov_q   mair, MAIR_EL1_SET
    mov_q   tcr, TCR_T0SZ(IDMAP_VA_BITS) | TCR_T1SZ(VA_BITS_MIN) | TCR_CACHE_FLAGS | \
             TCR_SHARED | TCR_TG_FLAGS | TCR_KASLR_FLAGS | TCR_EL1_AS | \
             TCR_EL1_TBI0 | TCR_EL1_A1 | TCR_KASAN_SW_FLAGS | TCR_MTE_FLAGS
    mov     tcr2, xzr
```

Meaning:
- reserve local names for temporary registers
- build `MAIR_EL1` value
- build default `TCR_EL1` value
- initialize `TCR2_EL1` working value to zero

Why:
- collect the kernel's baseline MMU policy before applying feature-specific modifications

## Block 4: apply errata workarounds

```asm
    tcr_clear_errata_bits tcr, x9, x5
```

Meaning:
- modify `tcr` if the running CPU matches an erratum pattern

Why:
- avoid known bad combinations of translation-control bits on affected hardware

## Block 5: optional 52-bit VA and LPA2 adaptation

```asm
#ifdef CONFIG_ARM64_VA_BITS_52
    mov     x9, #64 - VA_BITS
alternative_if ARM64_HAS_VA52
    tcr_set_t1sz tcr, x9
#ifdef CONFIG_ARM64_LPA2
    orr     tcr, tcr, #TCR_EL1_DS
#endif
alternative_else_nop_endif
#endif
```

Meaning:
- if the kernel was built for this path and the CPU supports it, adjust the top address-space sizing behavior
- optionally set the data-size related bit for LPA2 use

Why:
- let the same kernel adapt to wider virtual-address capability on supported CPUs

## Block 6: compute physical address size

```asm
    tcr_compute_pa_size tcr, #TCR_EL1_IPS_SHIFT, x5, x6
```

Meaning:
- read the CPU's physical address-range capability
- encode the result into the IPS field inside `TCR_EL1`

Why:
- the MMU must know how wide the physical address space is expected to be

## Block 7: optional hardware access-flag support

```asm
#ifdef CONFIG_ARM64_HW_AFDBM
    mrs     x9, ID_AA64MMFR1_EL1
    ubfx    x9, x9, ID_AA64MMFR1_EL1_HAFDBS_SHIFT, #4
    cbz     x9, 1f
    orr     tcr, tcr, #TCR_EL1_HA
#ifdef CONFIG_ARM64_HAFT
    cmp     x9, ID_AA64MMFR1_EL1_HAFDBS_HAFT
    b.lt    1f
    orr     tcr2, tcr2, TCR2_EL1_HAFT
#endif
1:
#endif
```

Meaning:
- query hardware support for access-flag management
- enable hardware access-flag update if supported
- optionally enable HAFT if the feature level is high enough

Why:
- allow hardware to participate in access tracking and related advanced behavior

## Block 8: commit `MAIR_EL1` and `TCR_EL1`

```asm
    msr     mair_el1, mair
    msr     tcr_el1, tcr
```

Meaning:
- write the prepared policy into the actual EL1 system registers

Why:
- make the MMU configuration ready before the final enable point

## Block 9: optional permission indirection setup

```asm
    mrs_s   x1, SYS_ID_AA64MMFR3_EL1
    ubfx    x1, x1, #ID_AA64MMFR3_EL1_S1PIE_SHIFT, #4
    cbz     x1, .Lskip_indirection

    mov_q   x0, PIE_E0_ASM
    msr     REG_PIRE0_EL1, x0
    mov_q   x0, PIE_E1_ASM
    msr     REG_PIR_EL1, x0

    orr     tcr2, tcr2, TCR2_EL1_PIE
```

Meaning:
- if S1PIE is supported, load the permission-indirection tables for EL0 and EL1 behavior
- record the enable bit into `tcr2`

Why:
- newer CPUs can translate permission indices through a more flexible hardware mechanism

## Block 10: optional `TCR2_EL1` write

```asm
    mrs_s   x1, SYS_ID_AA64MMFR3_EL1
    ubfx    x1, x1, #ID_AA64MMFR3_EL1_TCRX_SHIFT, #4
    cbz     x1, 1f
    msr     REG_TCR2_EL1, tcr2
1:
```

Meaning:
- only write the extended translation-control register if the CPU says it exists

Why:
- older CPUs may not implement this register interface

## Block 11: prepare final MMU-on control value

```asm
    mov_q   x0, INIT_SCTLR_EL1_MMU_ON
    ret
```

Meaning:
- place the final desired `SCTLR_EL1` value in `x0`
- return to the caller

Why:
- the caller will later install TTBR values and then use this return value to actually enable the MMU

## Final summary

`__cpu_setup` is best understood as a policy-loading function. It does not build the tables and it does not execute the final enable write. It loads the architectural rules that make the later enable step valid and safe.
