# ARM64 __cpu_setup Vault: Complete Documentation Index

**Generated**: Comprehensive documentation covering ARM64 CPU initialization and memory management setup

**Total Sections**: 25 complete documentation sections

---

## Foundation Sections (00-06): Background & Context

### [00_Reading_Guide](00_Reading_Guide/README.md)
How to navigate this vault and prerequisites for understanding __cpu_setup

### [01_ARMv8_Boot_Context](01_ARMv8_Boot_Context/README.md)
ARM64 architecture overview, boot sequence, execution contexts

### [02_Exception_Levels_and_Privilege](02_Exception_Levels_and_Privilege/README.md)
Exception levels (EL0-EL3), privilege model, system vs user space

### [03_VMSA_and_Address_Translation](03_VMSA_and_Address_Translation/README.md)
Virtual Memory System Architecture fundamentals, VA→PA translation principles

### [04_Page_Tables_and_Descriptors](04_Page_Tables_and_Descriptors/README.md)
Page table structures, descriptor formats, hierarchical translation

### [05_MMU_Control_Registers](05_MMU_Control_Registers/README.md)
System register architecture (TCR, TTBR, MAIR, SCTLR), register fields and meanings

### [06_Boot_Call_Flow](06_Boot_Call_Flow/README.md)
Who calls __cpu_setup, when, and what context it runs in

---

## Core Implementation Sections (07-19): Detailed Function Breakdown

### [07___cpu_setup_Contract_and_Placement](07___cpu_setup_Contract_and_Placement/README.md)
**Files**: `code_saying.md`, `why_this_exists.md`, `cpu_register_memory.md`, `diagrams.md`

Function signature, calling convention, input/output contracts, placement in .idmap.text section

- **code_saying.md**: Assembly code structure, function prologue/epilogue
- **why_this_exists.md**: Design rationale for universal CPU-agnostic VMSA setup
- **cpu_register_memory.md**: CPU pipeline effects, register visibility, memory coherency

### [08_TLB_Invalidate](08_TLB_Invalidate/README.md)
TLB invalidation (tlbi vmalle1) and data synchronization barriers

- Instruction encoding and binary representation
- Why TLB clear mandatory before TCR changes
- Barrier sequencing and coherency requirements

### [09_Control_State_Reset](09_Control_State_Reset/README.md)
Disabling coprocessor access (CPACR_EL1), debug control (MDSCR_EL1), PMU/AMU resets

- Threat models: uninitialized state leaks, side-channels, debug access
- Register field reset patterns
- Security hardening implications

### [10_Build_MAIR_and_TCR](10_Build_MAIR_and_TCR/README.md)
Memory Attribute Indirection Register setup - defines 8 memory attribute types

- Memory attribute encodings (Device, Normal cached, etc.)
- MAIR layout and indexing
- Page table attribute indirection mechanism

### [11_Errata_Scrub](11_Errata_Scrub/README.md)
CPU errata detection and workaround application

- CPU model identification via MIDR_EL1
- Conditional workaround application
- Common errata (Cortex-A72 #853709, A55 #1530923, Neoverse-N1 #1542419)

### [12_VA52_and_LPA2](12_VA52_and_LPA2/README.md)
52-bit VA support and Large Physical Address extensions

- TCR T0SZ/T1SZ computation for different address spaces
- Dynamic feature detection and configuration

### [13_IPS_and_PARange](13_IPS_and_PARange/README.md)
Intermediate Physical Size and Physical Address Range configuration

- CPU feature detection for physical address width
- Conditional TCR settings

### [14_Hardware_AF_and_HAFT](14_Hardware_AF_and_HAFT/README.md)
Hardware Access Flag support and Hardware AF Tracking

- Automatic page access tracking
- Performance implications of AF settings

### [15_Program_MAIR_and_TCR](15_Program_MAIR_and_TCR/README.md)
**Core sequence**: Actually writing MAIR_EL1 and TCR_EL1 registers

- Register write sequence
- Break-Before-Make (BBM) pattern
- Memory attribute configuration in practice

### [16_S1PIE_and_Permission_Indirection](16_S1PIE_and_Permission_Indirection/README.md)
Stage 1 Permission Indirection Extension (ARMv8.3+)

- Permission encoding indirection
- Conditional setup for CPUs supporting FPAC

### [17_TCR2_Write_Guard](17_TCR2_Write_Guard/README.md)
TCR2_EL1 configuration (ARMv8.2+ extension)

- Hardware Access Flag Tracking
- Permission Indirection Extension
- Execution Prevention Domain features

### [18_Return_SCTLR_Value](18_Return_SCTLR_Value/README.md)
**Return value preparation**: Computing INIT_SCTLR_EL1_MMU_ON value

- SCTLR_EL1 register layout
- Returned in x0 (not written in __cpu_setup)
- Caller's responsibility to actually enable MMU

### [19___enable_mmu_Handoff](19___enable_mmu_Handoff/README.md)
Coordination with caller for MMU enablement and boot flow

- Pre-call requirements (identity mapping, stack, exception vectors)
- Post-call responsibilities (page table building, SCTLR write)
- Complete boot choreography

---

## Reference & Summary Sections (20-24): Integration & Context

### [20_Secondary_CPU_and_Resume](20_Secondary_CPU_and_Resume/README.md)
**Files**: `code_saying.md`, `why_this_exists.md`, `cpu_register_memory.md`, `diagrams.md`

How __cpu_setup handles secondary CPU bringup and CPU suspend/resume

- Secondary CPU call site from secondary_startup
- Resume CPU call site from cpu_resume
- Register state restoration on resume
- Multi-CPU coordination and synchronization

### [21_Register_Atlas](21_Register_Atlas/README.md)
Complete reference of all registers modified by __cpu_setup

**Registers documented**:
- TTBR0_EL1, TTBR1_EL1: Translation Table Base
- TCR_EL1, TCR2_EL1: Translation Control
- MAIR_EL1: Memory Attribute Indirection
- SCTLR_EL1: System Control (return value only)
- CPACR_EL1: Coprocessor Access Control
- MDSCR_EL1: Monitor Debug System Control
- PMCR_EL0, PMCNTENSET_EL0: PMU control registers
- AMCFGR_EL0, AMCG1IDR_EL0: Activity Monitor configuration

### [22_Memory_Atlas](22_Memory_Atlas/README.md)
Memory layout and address space mapping during/after __cpu_setup

- .idmap.text section (identity-mapped boot code)
- Kernel memory layout assumptions
- Page table address ranges
- Virtual address space layout (TTBR0 vs TTBR1)

### [23_Mermaid_Diagrams](23_Mermaid_Diagrams/README.md)
Visual flowcharts and diagrams

- __cpu_setup execution flow
- MMU enable sequence
- TLB coherency barrier sequence
- Register dependency graphs
- CPU errata decision tree

### [24_Interview_and_Debug_Notes](24_Interview_and_Debug_Notes/README.md)
Frequently asked questions, debugging guides, and troubleshooting

**Topics covered**:
- "What happens if __cpu_setup is skipped?"
- "How do I debug a hanging __cpu_setup?"
- "What are common __cpu_setup bugs?"
- "How do I add support for new CPU variants?"
- "What's the performance impact of __cpu_setup?"

---

## Quick Reference

### Key Files in Linux Kernel

| File | Purpose |
|------|---------|
| arch/arm64/kernel/head.S | __cpu_setup definition and callers |
| arch/arm64/kernel/setup.c | Post-__cpu_setup C-level setup |
| arch/arm64/kernel/cpu_errata.c | Errata database and detection |
| arch/arm64/include/asm/sysreg.h | Register definitions |
| arch/arm64/include/asm/memory.h | Memory configuration macros |

### Critical Concepts

| Concept | Meaning | Section |
|---------|---------|---------|
| VMSA | Virtual Memory System Architecture | 03 |
| BBM | Break-Before-Make (TLB coherency) | 15, 19 |
| KPTI | Kernel Page Table Isolation (Meltdown fix) | 19 |
| .idmap.text | Identity-mapped boot code section | 07 |
| TCR | Translation Control Register | 12-15 |
| MAIR | Memory Attribute Indirection Register | 10 |
| Errata | CPU-specific bugs and workarounds | 11, 21 |

### Key Values (Typical)

```
INIT_SCTLR_EL1_MMU_ON:  0x3c5f83b1 (enables MMU with caching)
INIT_MAIR_EL1:          0xff443c0400 (8 memory attribute slots)
INIT_TCR_EL1:           Computed dynamically based on VA width
```

---

## Documentation Paths

Each major section (07-20) includes three complementary document types:

1. **code_saying.md**: Assembly code, binary encodings, register layouts
2. **why_this_exists.md**: Design rationale, threat models, architectural decisions
3. **cpu_register_memory.md**: CPU pipeline effects, memory coherency, hardware state

Plus optional:
- **README.md**: Section overview and key concepts
- **diagrams.md**: Flowcharts, timing diagrams, visual references

---

## How to Use This Vault

### For Understanding __cpu_setup
1. Start with **00_Reading_Guide** for prerequisites
2. Read **01-06** for architectural background
3. Deep dive **07-19** for implementation details
4. Reference **21-24** for specific information

### For Implementation/Debugging
1. Check **21_Register_Atlas** for register documentation
2. Reference **22_Memory_Atlas** for memory layout
3. Use **24_Interview_and_Debug_Notes** for troubleshooting
4. Consult **23_Mermaid_Diagrams** for execution flow

### For Contributing to Linux Kernel
1. Read **06_Boot_Call_Flow** to understand call sites
2. Study **11_Errata_Scrub** and **21_Register_Atlas** for architecture details
3. Use **24_Interview_and_Debug_Notes** for debugging new CPUs
4. Reference appropriate section from **07-19** for specific register operations

---

## Coverage Matrix

| Topic | Sections | Files |
|-------|----------|-------|
| Architecture background | 00-06 | 6 |
| TLB & synchronization | 08, 15, 19 | 3 |
| Memory management setup | 09-17 | 9 |
| Errata handling | 11 | 1 |
| Secondary CPU/resume | 20 | 1 |
| Reference material | 21-24 | 4 |
| **Total** | **25** | **25+** |

---

## Last Updated

Complete documentation generated and indexed. All 25 sections contain:
- Detailed technical explanations
- Assembly code examples
- Register field layouts
- Architecture specifications
- Debugging guides

**Status**: ✅ Complete and ready for reference

---

## Recommended Reading Order

**Quick Start** (30 minutes):
→ 00_Reading_Guide → 01 → 02 → 03 → 07 → 18 → 19

**Deep Dive** (2-3 hours):
→ Complete sequence 00-24 in order

**Reference Lookup** (5-10 minutes):
→ Jump to specific section number + consult INDEX for guidance

**Debugging** (Variable):
→ 24_Interview_and_Debug_Notes → 21_Register_Atlas → relevant section
