# ARM64 Linux `__cpu_setup` Master Learning Repo

This directory is a guided learning set for understanding the ARM64 Linux kernel function `__cpu_setup` from three angles:

- CPU view: what the core is doing before the MMU is enabled.
- Memory view: how translation, memory attributes, TLB state, and page tables are prepared.
- Kernel view: where `__cpu_setup` sits in the Linux boot flow and what code runs before and after it.

The target audience is a beginner. The documents start with fundamentals and then move toward line-by-line assembly analysis.

## What `__cpu_setup` does in one sentence

`__cpu_setup` prepares EL1 system registers so that the next stage can safely enable the MMU and transition into the kernel virtual address space.

## Recommended reading order

1. Start with `07-Reference/Reading-Guide.md`.
2. Read the foundational concepts in `01-Foundational-Concepts/`.
3. Read the boot flow documents in `03-CPU-Initialization-Sequence/`.
4. Study the function breakdown in `04-__cpu_setup-Deep-Dive/`.
5. Use `02-MMU-Architecture/` and `05-Memory-Management/` as supporting technical references.
6. Finish with `06-Connecting-the-Dots/` to connect CPU, MMU, board, and kernel behavior.

## Directory layout

- `01-Foundational-Concepts/`: beginner prerequisites.
- `02-MMU-Architecture/`: architectural meaning of MAIR, TCR, SCTLR, TLB, and related concepts.
- `03-CPU-Initialization-Sequence/`: execution-wise flow from boot entry to MMU enable.
- `04-__cpu_setup-Deep-Dive/`: direct deep study of the function and each block inside it.
- `05-Memory-Management/`: identity mapping, page tables, and kernel memory layout.
- `06-Connecting-the-Dots/`: end-to-end understanding and debugging mindset.
- `07-Reference/`: quick reference, glossary, source map, and reading paths.
- `diagrams/`: Mermaid source diagrams used by the documents.

## Main kernel files used in this repo

- `/linux/arch/arm64/mm/proc.S`
- `/linux/arch/arm64/kernel/head.S`
- `/linux/arch/arm64/include/asm/assembler.h`
- `linux/arch/arm64/include/asm/sysreg.h`
- `/linux/arch/arm64/include/asm/memory.h`
- `/linux/arch/arm64/include/asm/kernel-pgtable.h`
- `/linux/arch/arm64/include/asm/pgtable-prot.h`

## Core idea to keep in mind

`__cpu_setup` does not turn the MMU on by itself. It programs the control registers and returns the final `SCTLR_EL1` value in `x0`. The actual enable point happens later in `__enable_mmu`.

## Suggested study method

- First read for intent.
- Then read for register programming.
- Then read for memory translation impact.
- Finally return to the assembly listing and annotate every instruction.
