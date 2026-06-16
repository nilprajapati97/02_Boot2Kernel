
# __cpu_setup on ARMv8: Deep Walkthrough Index

This workspace is a from-scratch document set for understanding how the Linux arm64 kernel prepares EL1 to enable stage-1 translation during boot.

The center of the study is `__cpu_setup` in `arch/arm64/mm/proc.S`, but the explanation intentionally starts earlier than that function. If you only read `__cpu_setup` in isolation, you miss why the code lives in `.idmap.text`, why it returns an `SCTLR_EL1` value instead of enabling the MMU directly, why `TTBR0_EL1` and `TTBR1_EL1` are loaded later, and why resume and secondary CPUs reuse the same path.

## What This Set Covers

- ARMv8-A boot context at the architectural level
- Exception levels and why Linux settles at EL1 for the normal kernel
- VMSA stage-1 translation from virtual address to physical address
- Page-table structure, descriptors, memory attributes, and translation controls
- The boot call flow from `primary_entry` to `__primary_switched`
- A block-by-block walkthrough of `__cpu_setup`
- The `__enable_mmu` handoff where translation actually becomes live
- Reuse of the same setup path for secondary CPUs and suspend/resume
- Register atlas, memory atlas, Mermaid diagrams, and debug/interview notes

## Recommended Reading Order

1. [Walkthrough/00_Reading_Guide/README.md](Walkthrough/00_Reading_Guide/README.md)
2. [Walkthrough/01_ARMv8_Boot_Context/README.md](Walkthrough/01_ARMv8_Boot_Context/README.md)
3. [Walkthrough/02_Exception_Levels_and_Privilege/README.md](Walkthrough/02_Exception_Levels_and_Privilege/README.md)
4. [Walkthrough/03_VMSA_and_Address_Translation/README.md](Walkthrough/03_VMSA_and_Address_Translation/README.md)
5. [Walkthrough/04_Page_Tables_and_Descriptors/README.md](Walkthrough/04_Page_Tables_and_Descriptors/README.md)
6. [Walkthrough/05_MMU_Control_Registers/README.md](Walkthrough/05_MMU_Control_Registers/README.md)
7. [Walkthrough/06_Boot_Call_Flow/README.md](Walkthrough/06_Boot_Call_Flow/README.md)
8. [Walkthrough/07___cpu_setup_Contract_and_Placement/README.md](Walkthrough/07___cpu_setup_Contract_and_Placement/README.md)
9. [Walkthrough/08_TLB_Invalidate/README.md](Walkthrough/08_TLB_Invalidate/README.md)
10. [Walkthrough/09_Control_State_Reset/README.md](Walkthrough/09_Control_State_Reset/README.md)
11. [Walkthrough/10_Build_MAIR_and_TCR/README.md](Walkthrough/10_Build_MAIR_and_TCR/README.md)
12. [Walkthrough/11_Errata_Scrub/README.md](Walkthrough/11_Errata_Scrub/README.md)
13. [Walkthrough/12_VA52_and_LPA2/README.md](Walkthrough/12_VA52_and_LPA2/README.md)
14. [Walkthrough/13_IPS_and_PARange/README.md](Walkthrough/13_IPS_and_PARange/README.md)
15. [Walkthrough/14_Hardware_AF_and_HAFT/README.md](Walkthrough/14_Hardware_AF_and_HAFT/README.md)
16. [Walkthrough/15_Program_MAIR_and_TCR/README.md](Walkthrough/15_Program_MAIR_and_TCR/README.md)
17. [Walkthrough/16_S1PIE_and_Permission_Indirection/README.md](Walkthrough/16_S1PIE_and_Permission_Indirection/README.md)
18. [Walkthrough/17_TCR2_Write_Guard/README.md](Walkthrough/17_TCR2_Write_Guard/README.md)
19. [Walkthrough/18_Return_SCTLR_Value/README.md](Walkthrough/18_Return_SCTLR_Value/README.md)
20. [Walkthrough/19___enable_mmu_Handoff/README.md](Walkthrough/19___enable_mmu_Handoff/README.md)
21. [Walkthrough/20_Secondary_CPU_and_Resume/README.md](Walkthrough/20_Secondary_CPU_and_Resume/README.md)
22. [Walkthrough/21_Register_Atlas/README.md](Walkthrough/21_Register_Atlas/README.md)
23. [Walkthrough/22_Memory_Atlas/README.md](Walkthrough/22_Memory_Atlas/README.md)
24. [Walkthrough/23_Mermaid_Diagrams/README.md](Walkthrough/23_Mermaid_Diagrams/README.md)
25. [Walkthrough/24_Interview_and_Debug_Notes/README.md](Walkthrough/24_Interview_and_Debug_Notes/README.md)

## Core Contract

`__cpu_setup` is the arm64 kernel's early EL1 architectural bring-up routine. It prepares the translation regime and related execution controls so Linux can safely enable the MMU, but it does not itself install live page tables and does not itself perform the final `SCTLR_EL1` write that enables translation. It computes and programs `MAIR_EL1`, `TCR_EL1`, optionally `TCR2_EL1`, and a few other controls, then returns the desired final `SCTLR_EL1` value in `x0`. The caller later loads `TTBR0_EL1` and `TTBR1_EL1` and commits the final `SCTLR_EL1` write.

That design split is one of the most important ideas in this topic.

## Source Anchors Used For These Documents

- `\linux\arch\arm64\mm\proc.S`
- `\linux\arch\arm64\kernel\head.S`
- `\linux\arch\arm64\kernel\sleep.S`
- `\linux\arch\arm64\include\asm\assembler.h`
- `\linux\arch\arm64\include\asm\memory.h`
- `\linux\arch\arm64\include\asm\sysreg.h`

## Quick Glossary

- PE: Processing Element, effectively the architected CPU core executing instructions
- EL1: Privilege level where the normal Linux kernel runs
- VMSA: Virtual Memory System Architecture
- TTBR: Translation Table Base Register
- MAIR: Memory Attribute Indirection Register
- TCR: Translation Control Register
- SCTLR: System Control Register
- IPS: Intermediate Physical Address Size field carried in `TCR_EL1`
- AF: Access Flag
- idmap: Identity mapping used while the kernel is not yet executing under the final virtual layout

## Best Starting Points By Goal

- Beginner: start at 00 through 06, then read 07 before any per-block chapter
- Hardware deep dive: read 03, 04, 05, 10, 12, 13, 14, 16, 19, 21, and 22
- Interview prep: read 07, then 08 through 20, then finish with 24
- Visual learner: read 06 and 23 early, then return to the deeper chapters