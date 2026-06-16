# 00 Reading Guide

This document set is structured as a staircase.

The first six chapters build the minimum architectural model needed to read early arm64 boot code without guessing. Only after that does the walkthrough move into `__cpu_setup` itself. This matters because the function is small in instruction count but very dense in architectural meaning.

## How To Read This Set

Read each document in order if you are new to ARMv8-A MMU bring-up. If you already know the architecture but need Linux-specific behavior, jump from chapter 06 to chapter 07 and then read the `__cpu_setup` chapters.

Each per-block chapter uses the same frame:

- What instructions or macros are involved
- What architectural register or structure is being touched
- What hardware behavior changes because of it
- Why Linux does it in that location in the boot sequence
- What can break if the step is wrong or missing

## Mental Model To Keep Throughout

The boot path is not just turning on a switch called the MMU. The kernel is changing the execution model of the core.

Before the final handoff:

- code is executing from an identity-mapped region
- the final kernel virtual address layout is not yet trusted
- old firmware state may still exist in architectural registers
- the CPU must be brought to a deterministic EL1 baseline

After the handoff:

- `TTBR0_EL1` and `TTBR1_EL1` point at Linux-owned page tables
- `TCR_EL1` and `MAIR_EL1` describe how addresses are interpreted and how memory types behave
- `SCTLR_EL1.M` makes that regime live

## Suggested Study Pattern

1. Build the address-translation model first.
2. Understand why Linux separates setup from enable.
3. Read each `__cpu_setup` block as a change to one part of that model.
4. End with `__enable_mmu`, because that is where the prepared model becomes active.

## What To Watch For

- Distinguish architecture rules from Linux policy.
- Distinguish compile-time capability from runtime-detected feature support.
- Distinguish data structure setup from activation of those structures.

Those three distinctions explain most of the design choices in this path.