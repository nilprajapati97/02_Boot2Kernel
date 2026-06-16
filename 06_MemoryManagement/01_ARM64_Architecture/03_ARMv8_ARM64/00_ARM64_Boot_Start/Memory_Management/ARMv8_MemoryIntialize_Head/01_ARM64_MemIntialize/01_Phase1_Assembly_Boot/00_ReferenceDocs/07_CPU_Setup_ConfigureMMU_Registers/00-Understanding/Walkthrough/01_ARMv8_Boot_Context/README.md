# 01 ARMv8 Boot Context

To understand `__cpu_setup`, start with the architectural problem Linux is solving.

## What The CPU Looks Like At Entry

When the kernel first executes, the processing element may arrive from firmware at EL2 or EL1 depending on platform policy. The MMU may be on or off. Caches may be on or off. Some architected registers may still contain firmware choices. Linux cannot assume any of that state is already aligned with the kernel's own stage-1 translation regime.

That is why the early arm64 path in `head.S` first records state, preserves boot arguments, creates or validates the identity-map tables, normalizes exception-level state, and only then calls `__cpu_setup`.

## Why Early Boot Code Is Special

Early boot runs under three constraints:

- the normal kernel virtual address layout is not yet active
- the code must remain executable while translation state is changing
- architectural state inherited from firmware must not be trusted blindly

This is why Linux uses `.idmap.text` for the low-level routines that participate in the handoff.

## What Linux Needs Before Enabling The MMU

Before `SCTLR_EL1.M` can be set safely, Linux must know:

- what memory types each page-table `AttrIndx` value means
- how large the input and output address spaces are
- what translation granule is in use
- how page-table walks should treat cacheability and shareability
- whether optional features like VA52, HAFT, or permission indirection are legal on this CPU
- what `SCTLR_EL1` bits should be committed when translation becomes live

`__cpu_setup` is where Linux assembles that answer.

## The Hardware-Level Perspective

At the hardware level, enabling the MMU is not only address translation. It affects:

- page-walk format
- page-walk cacheability
- translation lookaside buffer population and invalidation requirements
- interpretation of leaf descriptors
- tagged-address behavior
- stack alignment checks and a group of execution controls carried in `SCTLR_EL1`

So this path is properly understood as architectural bring-up, not just memory initialization.