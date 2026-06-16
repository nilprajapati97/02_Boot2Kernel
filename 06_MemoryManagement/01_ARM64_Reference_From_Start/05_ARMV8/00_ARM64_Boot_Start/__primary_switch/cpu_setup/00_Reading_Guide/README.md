# 00_Reading_Guide

## Scope
How to study this ARMv8-A vault from zero to advanced.

## Learning Objectives
- Understand this stage in ARMv8-A AArch64 Linux boot.
- Connect each operation back to __cpu_setup behavior.
- Explain CPU-level, register-level, and memory-level impact.
- Recognize what can fail if this step is skipped.

## Source Anchors
- arch/arm64/mm/proc.S (__cpu_setup)
- arch/arm64/kernel/head.S (boot call flow and __enable_mmu)
- arch/arm64/include/asm/assembler.h (macros)
- arch/arm64/include/asm/pgtable-hwdef.h (TCR fields)
- arch/arm64/include/asm/sysreg.h (system register definitions)

## Quick Start
1. Read code_saying.md
2. Read why_this_exists.md
3. Read cpu_register_memory.md
4. Read diagrams.md and trace the path mentally
