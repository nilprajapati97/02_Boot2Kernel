# 14 Hardware AF And `HAFT`

This block uses `ID_AA64MMFR1_EL1.HAFDBS` to decide whether the hardware can participate in access-flag management.

## What Linux Enables

If the CPU advertises support, Linux sets `TCR_EL1.HA` so the hardware can update the access flag in translation-table state.

If the CPU advertises the higher HAFT capability and the kernel is configured for it, Linux also sets `TCR2_EL1.HAFT`.

## Why This Matters

The access flag is part of the memory-management bookkeeping model. Hardware support means the MMU can set access state directly instead of forcing the kernel to manage all of it by software fault handling.

## Important Nuance

The boot code comment explicitly distinguishes hardware access-flag update from hardware dirty-bit policy. Linux only enables the early-safe subset here. More complete policy decisions are finalized later through the kernel capability machinery.

## Big Picture

This is a good example of Linux enabling an immediately useful architectural facility while deferring broader MM policy to later boot phases.