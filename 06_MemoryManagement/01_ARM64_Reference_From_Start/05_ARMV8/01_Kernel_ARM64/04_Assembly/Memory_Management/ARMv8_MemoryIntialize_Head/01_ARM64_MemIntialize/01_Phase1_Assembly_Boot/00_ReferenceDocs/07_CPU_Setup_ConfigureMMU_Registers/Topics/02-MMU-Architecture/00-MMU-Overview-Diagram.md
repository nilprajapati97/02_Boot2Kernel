# MMU Overview

The MMU is the hardware block that translates virtual addresses to physical addresses and applies memory attributes and protection rules.

## What the MMU needs before it can be enabled

- valid translation tables
- translation control configuration
- memory attribute definitions
- translation base register values
- a final control-register write that turns translation on

## Where `__cpu_setup` fits

`__cpu_setup` contributes the middle pieces:

- `MAIR_EL1`
- `TCR_EL1`
- optional `TCR2_EL1`
- returned `SCTLR_EL1` value

## MMU preparation picture

```mermaid
flowchart LR
    A[Early boot] --> B[Build idmap page tables]
    B --> C[Run __cpu_setup]
    C --> D[Load TTBR0 and TTBR1]
    D --> E[Write SCTLR_EL1]
    E --> F[MMU on]
```

## One-sentence summary

The MMU does not turn on just because page tables exist. The CPU must be told how to use them, and that is why `__cpu_setup` matters.
