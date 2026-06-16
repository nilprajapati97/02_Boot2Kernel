# Reading Guide

This guide gives multiple learning paths depending on your current level.

## Path 1: Complete beginner

1. `../01-Foundational-Concepts/00-ARM64-Overview.md`
2. `../01-Foundational-Concepts/01-CPU-Modes-EL-Levels.md`
3. `../01-Foundational-Concepts/02-Virtual-Memory-Basics.md`
4. `../01-Foundational-Concepts/03-ARM64-Register-Model.md`
5. `../01-Foundational-Concepts/04-System-Registers-101.md`
6. `../01-Foundational-Concepts/05-ARM64-Assembly-Cheat-Sheet.md`
7. `../03-CPU-Initialization-Sequence/00-Boot-Flow-Overview.md`
8. `../03-CPU-Initialization-Sequence/03-__cpu_setup-Phase.md`
9. `../04-__cpu_setup-Deep-Dive/00-Function-Overview.md`
10. `../04-__cpu_setup-Deep-Dive/07-Code-Walkthrough.md`

## Path 2: Already know ARM64 basics

1. `../03-CPU-Initialization-Sequence/00-Boot-Flow-Overview.md`
2. `../03-CPU-Initialization-Sequence/01-Primary-Entry-Phase.md`
3. `../03-CPU-Initialization-Sequence/03-__cpu_setup-Phase.md`
4. `../04-__cpu_setup-Deep-Dive/02-Register-Initialization.md`
5. `../04-__cpu_setup-Deep-Dive/07-Code-Walkthrough.md`

## Path 3: Only want the exact function

1. `Kernel-Source-Map.md`
2. `Glossary.md`
3. `../04-__cpu_setup-Deep-Dive/00-Function-Overview.md`
4. `../04-__cpu_setup-Deep-Dive/07-Code-Walkthrough.md`
5. `../02-MMU-Architecture/03-Translation-Control-Regs.md`
6. `../02-MMU-Architecture/04-System-Control-Register.md`

## Path 4: Interested in memory-management angle

1. `../01-Foundational-Concepts/02-Virtual-Memory-Basics.md`
2. `../05-Memory-Management/01-Identity-Mapping.md`
3. `../05-Memory-Management/00-Page-Table-Setup.md`
4. `../02-MMU-Architecture/02-Memory-Attribute-Registers.md`
5. `../02-MMU-Architecture/03-Translation-Control-Regs.md`
6. `../06-Connecting-the-Dots/00-From-Zero-to-Paging.md`

## What to focus on while reading

- Which register is being prepared.
- Whether the change affects CPU behavior, memory behavior, or both.
- Whether the code is compile-time conditional, runtime conditional, or unconditional.
- Whether the MMU is still off or already on.
- Which page tables are active at that point: idmap or swapper.

## Minimum mental model before reading `__cpu_setup`

You should know these five terms:

- Exception level
- TLB
- Page table
- Virtual address versus physical address
- System register
