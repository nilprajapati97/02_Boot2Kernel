# Early MMU Operations

Once `__cpu_setup` has finished, the boot path is close to actual MMU activation.

## What the next code does

- validate configured page-granule support
- load translation table base registers
- write `SCTLR_EL1`
- synchronize the CPU pipeline and cache view
- continue execution in the mapped kernel context

## Why this document exists

Many beginners over-credit `__cpu_setup` for the whole transition. The truth is that early MMU bring-up is a multi-stage process:

1. build tables
2. program policy registers
3. install table base registers
4. enable the MMU
5. continue in the intended mapped environment
