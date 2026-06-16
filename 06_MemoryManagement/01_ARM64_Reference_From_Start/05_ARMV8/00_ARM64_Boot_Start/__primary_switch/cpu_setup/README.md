# ARMv8-A __cpu_setup Study Vault

This vault explains Linux ARM64 __cpu_setup from scratch with CPU-level, register-level, and memory-level reasoning.

## Subdirectories
- [00_Reading_Guide](./00_Reading_Guide/README.md)
- [01_ARMv8_Boot_Context](./01_ARMv8_Boot_Context/README.md)
- [02_Exception_Levels_and_Privilege](./02_Exception_Levels_and_Privilege/README.md)
- [03_VMSA_and_Address_Translation](./03_VMSA_and_Address_Translation/README.md)
- [04_Page_Tables_and_Descriptors](./04_Page_Tables_and_Descriptors/README.md)
- [05_MMU_Control_Registers](./05_MMU_Control_Registers/README.md)
- [06_Boot_Call_Flow](./06_Boot_Call_Flow/README.md)
- [07___cpu_setup_Contract_and_Placement](./07___cpu_setup_Contract_and_Placement/README.md)
- [08_TLB_Invalidate](./08_TLB_Invalidate/README.md)
- [09_Control_State_Reset](./09_Control_State_Reset/README.md)
- [10_Build_MAIR_and_TCR](./10_Build_MAIR_and_TCR/README.md)
- [11_Errata_Scrub](./11_Errata_Scrub/README.md)
- [12_VA52_and_LPA2](./12_VA52_and_LPA2/README.md)
- [13_IPS_and_PARange](./13_IPS_and_PARange/README.md)
- [14_Hardware_AF_and_HAFT](./14_Hardware_AF_and_HAFT/README.md)
- [15_Program_MAIR_and_TCR](./15_Program_MAIR_and_TCR/README.md)
- [16_S1PIE_and_Permission_Indirection](./16_S1PIE_and_Permission_Indirection/README.md)
- [17_TCR2_Write_Guard](./17_TCR2_Write_Guard/README.md)
- [18_Return_SCTLR_Value](./18_Return_SCTLR_Value/README.md)
- [19___enable_mmu_Handoff](./19___enable_mmu_Handoff/README.md)
- [20_Secondary_CPU_and_Resume](./20_Secondary_CPU_and_Resume/README.md)
- [21_Register_Atlas](./21_Register_Atlas/README.md)
- [22_Memory_Atlas](./22_Memory_Atlas/README.md)
- [23_Mermaid_Diagrams](./23_Mermaid_Diagrams/README.md)
- [24_Interview_and_Debug_Notes](./24_Interview_and_Debug_Notes/README.md)

## File Pattern In Every Section
- README.md
- code_saying.md
- why_this_exists.md
- cpu_register_memory.md
- diagrams.md

## Study Order
1. 00 to 06 for foundations
2. 07 to 19 for direct __cpu_setup internals
3. 20 to 24 for integration, atlas, and debug readiness
