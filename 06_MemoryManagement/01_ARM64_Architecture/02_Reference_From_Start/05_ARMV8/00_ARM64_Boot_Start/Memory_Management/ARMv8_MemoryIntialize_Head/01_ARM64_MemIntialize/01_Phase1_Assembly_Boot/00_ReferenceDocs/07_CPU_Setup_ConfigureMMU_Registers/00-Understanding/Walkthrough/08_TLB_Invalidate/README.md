# 08 TLB Invalidate

The function begins with:

```asm
tlbi vmalle1
dsb nsh
```

## What It Means

`tlbi vmalle1` invalidates EL1 stage-1 translation entries for the local processing element. The following `dsb nsh` ensures that the invalidation is complete with respect to the core before execution continues.

## Hardware Reasoning

The TLB is a cache of translation results. If Linux changes `TCR_EL1`, switches tables, or changes how descriptors are interpreted while old TLB lines remain usable, the core may continue translating with stale information.

That is architecturally dangerous because the stale entries can reflect:

- old firmware mappings
- old walk attributes
- old address-size assumptions
- old permissions

## Why Local Only Here

At this stage the path is per-CPU. Linux is bringing one PE into a known state before the kernel's own translation regime becomes active for that PE. Full SMP-wide coordination is not the point of this local bring-up routine.

## Failure Mode If Omitted

You can get early boot hangs, silent execution through wrong mappings, or faults that make no sense relative to the page tables Linux believes it installed.