# Paging Initialization — Kernel Page Table Setup

## Overview

`paging_init()` (called from `setup_arch()`) sets up the **final kernel page tables** that remain in use for the entire kernel lifetime. Before this, the kernel runs on a minimal identity-mapped page table from the assembly prologue.

---

## x86-64 Virtual Address Space Layout (4-Level Paging)

```
Virtual Address Space (48-bit, 256TB total):

User space (lower half):
0x0000_0000_0000_0000 - 0x0000_7FFF_FFFF_FFFF  (128TB)

Kernel space (upper half, accessible only at CPL=0):
0xFFFF_8000_0000_0000 - 0xFFFF_87FF_FFFF_FFFF  Guard hole (invalid canonical)
0xFFFF_8880_0000_0000 - 0xFFFF_C87F_FFFF_FFFF  Direct mapping of physical RAM (64TB)
0xFFFF_C900_0000_0000 - 0xFFFF_E8FF_FFFF_FFFF  vmalloc/ioremap space (32TB)
0xFFFF_E900_0000_0000 - 0xFFFF_E9FF_FFFF_FFFF  vmemmap (struct page array, 1TB)
0xFFFF_EA00_0000_0000 - 0xFFFF_EAFF_FFFF_FFFF  KASAN shadow memory (1TB)
0xFFFF_FEFF_FFFF_F000 - 0xFFFF_FF00_FFFF_FFFF  Fixmap area
0xFFFF_FF00_0000_0000 - 0xFFFF_FF7F_FFFF_FFFF  %esp fixup stacks
0xFFFF_FFFF_8000_0000 - 0xFFFF_FFFF_9FFF_FFFF  Kernel text, data, BSS (~512MB)
0xFFFF_FFFF_A000_0000 - 0xFFFF_FFFF_DFFF_FFFF  Modules (1GB)
```

---

## Page Table Structure (x86-64, 4-Level)

```
CR3 ─► PGD (Page Global Directory)   [512 entries, 8B each = 4KB]
           │
           ▼ [PGD index from VA bits 47:39]
        PUD (Page Upper Directory)    [512 entries]
           │
           ▼ [PUD index from VA bits 38:30]
        PMD (Page Middle Directory)   [512 entries]
           │
           ▼ [PMD index from VA bits 29:21]
        PTE (Page Table Entry)        [512 entries]
           │
           ▼ [PTE index from VA bits 20:12]
        Physical Page                 [4KB page, offset from VA bits 11:0]
```

### Huge Pages
- **1GB huge pages** (1GHP): PUD entry directly points to 1GB physical region
- **2MB huge pages** (THP): PMD entry directly points to 2MB physical region
- Reduces TLB pressure dramatically — critical for NVIDIA GPU driver's VRAM management

---

## Direct Mapping Setup

```c
// arch/x86/mm/init.c
void __init init_mem_mapping(void)
{
    // Map all physical RAM at PAGE_OFFSET using 1GB or 2MB pages where possible
    // Falls back to 4KB pages at region boundaries
    
    unsigned long end = max_pfn << PAGE_SHIFT;
    
    // Use 1GB huge pages from 1GB-aligned base to end
    // Use 2MB pages for 2MB-aligned remainder
    // Use 4KB pages for odd-size remainder at top
    
    memory_map_bottom_up(ISA_END_ADDRESS, end);
}
```

### Why Use Huge Pages for Direct Mapping?

A 256TB direct mapping with 4KB pages would require:
- `256TB / 4KB = 64 billion PTEs × 8B = 512GB of page tables`

With 1GB pages:
- `256TB / 1GB = 262,144 PUD entries × 8B = ~2MB of page tables`

This is why the kernel uses the largest page size possible for the direct mapping.

---

## 5-Level Paging (x86-64)

Recent servers have more than 128TB RAM, requiring 5-level paging:
- Adds a P4D level between PGD and PUD
- Extends virtual address space to 57 bits (128PB)
- Enabled with `CONFIG_X86_5LEVEL` and CPU support (`LA57` CPUID bit)
- Required for AMD's EPYC servers with 4TB+ RAM (used at Google, AWS)

---

## ARM64 Paging

ARM64 uses similar 4-level page tables but with different register names:
- PGD → PUD → PMD → PTE
- Controlled by TTBR0_EL1 (user) and TTBR1_EL1 (kernel)
- Supports 4KB, 16KB, or 64KB page sizes (configured at compile time)
- Qualcomm uses 4KB pages; Apple M1 uses 16KB pages

---

## Interview Q&A

### Q1: What is a TLB and why does it matter for performance?
**A:** The TLB (Translation Lookaside Buffer) is a hardware cache that stores recent virtual→physical address translations. Without TLB, every memory access requires 4 memory accesses to walk the page table (PGD→PUD→PMD→PTE→data). With TLB, the translation is a single cycle. TLB capacity is limited (~1500 entries for 4KB pages on modern x86). If a process accesses more unique pages than the TLB can hold, "TLB thrashing" occurs — every access walks the full page table. Using 2MB huge pages gives 512x fewer TLB entries needed for the same memory range.

### Q2: What is KPTI (Kernel Page Table Isolation) and why was it introduced?
**A:** KPTI was introduced in Linux 4.15 as a fix for Meltdown (CVE-2017-5754). Meltdown exploits speculative execution: even when userspace can't architecturally read kernel memory, the CPU speculatively executes kernel reads and the data enters the cache — leakable via cache timing (Spectre/Flush+Reload). KPTI maintains **two separate page tables per CPU**: one for userspace (with no kernel mappings except a minimal syscall trampoline) and one for kernel mode (with full kernel mappings). Context switching flushes the TLB (or uses PCID for efficiency). Performance impact: 5-30% for syscall-heavy workloads.

### Q3: How does the kernel's direct mapping interact with DMA?
**A:** For DMA to work, the kernel gives device drivers physical addresses to program into DMA controllers. `virt_to_phys(ptr)` = `__pa(ptr)` converts a kernel virtual address (in the direct map) to its physical address. `dma_map_single()` may translate this further for IOMMU-protected DMA. The device DMA-reads/writes directly to physical memory. Because the direct mapping is 1:1 with physical addresses, `phys_to_virt(dma_addr)` gives the CPU's view of the DMA buffer — used for zero-copy network buffers (SKB data areas), NVIDIA GPU DMA descriptor rings, etc.
