# ARM32 MMU Architecture - Detailed Design Document
## Document 1: Architecture Overview and Fundamentals

**Author:** Senior Kernel Engineer  
**Target Architecture:** ARMv7-A (ARM32)  
**Scope:** Bare-metal and Linux Kernel Implementation  
**Revision:** 1.0  
**Date:** 2024

---

## Table of Contents
1. [Executive Summary](#executive-summary)
2. [MMU Architecture Overview](#mmu-architecture-overview)
3. [Translation Table Formats](#translation-table-formats)
4. [Memory Attributes and Access Control](#memory-attributes-and-access-control)
5. [TLB Architecture](#tlb-architecture)
6. [System Control Registers](#system-control-registers)

---

## 1. Executive Summary

### 1.1 Purpose
This document provides comprehensive technical specifications for implementing and managing the Memory Management Unit (MMU) in ARM32 (ARMv7-A) architecture. It covers both bare-metal initialization and kernel-level memory management.

### 1.2 Key Features
- **Virtual Memory Management**: 32-bit virtual address space (4GB)
- **Translation Granularity**: 4KB, 64KB, 1MB, 16MB sections
- **Two-Level Page Tables**: L1 (4096 entries) and L2 (256 entries)
- **Hardware TLB**: Separate instruction and data TLBs
- **Memory Protection**: Domain-based and AP-based access control
- **Cache Control**: Integrated with MMU for coherency

### 1.3 Design Goals
- **Performance**: Minimize TLB misses and page table walks
- **Security**: Enforce privilege separation and memory isolation
- **Flexibility**: Support multiple page sizes and memory types
- **Reliability**: Ensure cache coherency and atomic operations

---

## 2. MMU Architecture Overview

### 2.1 Virtual to Physical Address Translation

```
Virtual Address (32-bit)
┌─────────────────────────────────────────────────────────┐
│ [31:20] L1 Index │ [19:12] L2 Index │ [11:0] Page Offset│
└─────────────────────────────────────────────────────────┘
     12 bits            8 bits              12 bits

Translation Process:
1. Extract L1 index from VA[31:20]
2. Access TTBR0/TTBR1 + (L1_index << 2)
3. Read L1 descriptor
4. If Section: PA = descriptor[31:20] + VA[19:0]
5. If Page Table: Extract L2 base address
6. Access L2 base + (L2_index << 2)
7. Read L2 descriptor
8. PA = descriptor[31:12] + VA[11:0]
```

### 2.2 Translation Table Base Registers

#### TTBR0 (Translation Table Base Register 0)
- **Purpose**: User space translations (typically VA 0x00000000 - 0x7FFFFFFF)
- **Format**: `[31:14] Base Address | [13:7] Reserved | [6] IRGN[0] | [5] NOS | [4:3] RGN | [2] IMP | [1] S | [0] IRGN[1]`
- **Context Switch**: Updated on process switch

#### TTBR1 (Translation Table Base Register 1)
- **Purpose**: Kernel space translations (typically VA 0x80000000 - 0xFFFFFFFF)
- **Format**: Same as TTBR0
- **Context Switch**: Remains constant across process switches

#### TTBCR (Translation Table Base Control Register)
- **N field [2:0]**: Defines split between TTBR0 and TTBR1
  - N=0: TTBR0 covers 0x00000000-0xFFFFFFFF (no split)
  - N=1: TTBR0 covers 0x00000000-0x7FFFFFFF, TTBR1 covers 0x80000000-0xFFFFFFFF
  - N=2: TTBR0 covers 0x00000000-0x3FFFFFFF, TTBR1 covers 0xC0000000-0xFFFFFFFF
- **PD0/PD1**: Page table walk disable bits

### 2.3 Address Space Layout

```
Typical Linux ARM32 Layout:

0xFFFFFFFF ┌─────────────────────┐
           │   Kernel Vectors    │ (4KB)
0xFFFF0000 ├─────────────────────┤
           │   Kernel Modules    │ (16MB)
0xBF000000 ├─────────────────────┤
           │   vmalloc/ioremap   │ (240MB)
0xC0000000 ├─────────────────────┤ ← TTBR1 boundary
           │   Kernel Image      │
           │   .text, .data      │
           │   Direct Mapping    │
0x80000000 ├─────────────────────┤
           │   User Stack        │
           │        ↓            │
           │                     │
           │        ↑            │
           │   User Heap         │
           │   User .data/.bss   │
           │   User .text        │
0x00000000 └─────────────────────┘ ← TTBR0 boundary
```

---

## 3. Translation Table Formats

### 3.1 Level 1 Descriptor Types

#### 3.1.1 Invalid/Fault Entry
```
Bits:  [31:2] Ignored | [1:0] = 0b00
Purpose: Generate translation fault
```

#### 3.1.2 Page Table Entry
```
Bits:  [31:10] L2 Table Base Address (1KB aligned)
       [9]     NS (Non-Secure bit)
       [8:5]   Domain (0-15)
       [4]     SBZ (Should Be Zero)
       [3]     NS (Non-Secure)
       [2]     PXN (Privileged Execute Never)
       [1:0]   = 0b01 (Page Table)

Size: Points to 256-entry L2 table (1KB)
Coverage: 1MB of virtual address space
```

#### 3.1.3 Section Entry (1MB)
```
Bits:  [31:20] Section Base Address (1MB aligned)
       [19]    NS (Non-Secure)
       [18]    nG (not Global)
       [17]    S (Shareable)
       [16]    APX (Access Permission Extension)
       [15]    TEX[2]
       [14:12] TEX[1:0]
       [11:10] AP[1:0] (Access Permission)
       [9]     IMP (Implementation defined)
       [8:5]   Domain
       [4]     XN (Execute Never)
       [3]     C (Cacheable)
       [2]     B (Bufferable)
       [1:0]   = 0b10 (Section)

Size: 1MB
Virtual Coverage: 1MB
```

#### 3.1.4 Supersection Entry (16MB)
```
Bits:  [31:24] Supersection Base Address[31:24]
       [23:20] Supersection Base Address[39:36] (LPAE)
       [19]    NS
       [18]    nG
       [17]    S
       [16]    APX
       [15]    TEX[2]
       [14:12] TEX[1:0]
       [11:10] AP[1:0]
       [9]     IMP
       [8:5]   Extended Base Address[35:32]
       [4]     XN
       [3]     C
       [2]     B
       [1:0]   = 0b10 (Section with bit[18]=1)

Size: 16MB
Virtual Coverage: 16MB
```

### 3.2 Level 2 Descriptor Types

#### 3.2.1 Large Page Entry (64KB)
```
Bits:  [31:16] Large Page Base Address (64KB aligned)
       [15]    XN (Execute Never)
       [14:12] TEX[2:0]
       [11]    nG (not Global)
       [10]    S (Shareable)
       [9]     APX
       [8:6]   SBZ
       [5:4]   AP[1:0]
       [3]     C (Cacheable)
       [2]     B (Bufferable)
       [1:0]   = 0b01 (Large Page)

Size: 64KB
Virtual Coverage: 64KB
Entries Required: 16 consecutive identical entries in L2 table
```

#### 3.2.2 Small Page Entry (4KB)
```
Bits:  [31:12] Small Page Base Address (4KB aligned)
       [11]    nG (not Global)
       [10]    S (Shareable)
       [9]     APX
       [8:6]   TEX[2:0]
       [5:4]   AP[1:0]
       [3]     C (Cacheable)
       [2]     B (Bufferable)
       [1]     = 1 (Small Page)
       [0]     XN (Execute Never)

Size: 4KB
Virtual Coverage: 4KB
Most Common: Standard page size in Linux
```

### 3.3 Descriptor Selection Strategy

| Use Case | Recommended Descriptor | Rationale |
|----------|----------------------|-----------|
| Kernel Image | 1MB Sections | Fast translation, reduced TLB pressure |
| Device Memory | 1MB Sections | Large MMIO regions, simple mapping |
| User Pages | 4KB Small Pages | Fine-grained control, demand paging |
| Framebuffer | 64KB Large Pages | Balance between granularity and TLB |
| Kernel Modules | 4KB Small Pages | Dynamic loading, precise control |

---

## 4. Memory Attributes and Access Control

### 4.1 TEX, C, B Encoding (Memory Type)

#### Without Remapping (SCTLR.TRE = 0)

| TEX | C | B | Memory Type | Cacheable | Description |
|-----|---|---|-------------|-----------|-------------|
| 000 | 0 | 0 | Strongly-ordered | No | Device, strict ordering |
| 000 | 0 | 1 | Shared Device | No | Device, no reordering |
| 000 | 1 | 0 | Write-Through | Yes | Normal, WT, no WA |
| 000 | 1 | 1 | Write-Back | Yes | Normal, WB, no WA |
| 001 | 0 | 0 | Shareable Device | No | Device, shareable |
| 001 | 0 | 1 | Shareable Device | No | Device, shareable |
| 001 | 1 | 0 | Write-Through | Yes | Normal, WT, WA |
| 001 | 1 | 1 | Write-Back | Yes | Normal, WB, WA |
| 010-111 | x | x | Cacheable | Varies | See extended table |

#### Common Configurations

```c
/* Strongly-ordered (Device registers) */
#define MMU_DEVICE_STRONGLY_ORDERED  (0x0 << 12) | (0 << 3) | (0 << 2)

/* Device memory (MMIO) */
#define MMU_DEVICE_SHARED            (0x0 << 12) | (0 << 3) | (1 << 2)

/* Normal memory, Write-Back, Write-Allocate */
#define MMU_NORMAL_WB_WA             (0x1 << 12) | (1 << 3) | (1 << 2)

/* Normal memory, Write-Through */
#define MMU_NORMAL_WT                (0x0 << 12) | (1 << 3) | (0 << 2)

/* Non-cacheable normal memory */
#define MMU_NORMAL_NC                (0x1 << 12) | (0 << 3) | (0 << 2)
```

### 4.2 Access Permission (AP, APX)

#### AP[2:0] Encoding (AP[2] = APX, AP[1:0] from descriptor)

| APX | AP[1:0] | Privileged | Unprivileged | Description |
|-----|---------|------------|--------------|-------------|
| 0   | 00      | No Access  | No Access    | Reserved |
| 0   | 01      | RW         | No Access    | Kernel only |
| 0   | 10      | RW         | RO           | Kernel RW, User RO |
| 0   | 11      | RW         | RW           | Full access |
| 1   | 00      | Reserved   | Reserved     | Reserved |
| 1   | 01      | RO         | No Access    | Kernel RO |
| 1   | 10      | RO         | RO           | Read-only |
| 1   | 11      | RO         | RO           | Read-only (deprecated) |

#### Simplified Access Permission (SCTLR.AFE = 1)

| AP[2:1] | Privileged | Unprivileged | Description |
|---------|------------|--------------|-------------|
| 00      | RW         | No Access    | Kernel only |
| 01      | RW         | RW           | Full access |
| 10      | RO         | No Access    | Kernel RO |
| 11      | RO         | RO           | Read-only |

### 4.3 Domain Access Control

#### DACR (Domain Access Control Register)
- **32-bit register**: 16 domains × 2 bits each
- **Domain 0-15**: Each domain has 2-bit access control

#### Domain Access Types
```
0b00 = No Access (Fault)
0b01 = Client (Check AP bits)
0b10 = Reserved
0b11 = Manager (No AP check, full access)
```

#### Typical Usage
```c
/* Linux kernel typical setup */
#define DOMAIN_KERNEL    0  /* Kernel memory */
#define DOMAIN_USER      1  /* User memory */
#define DOMAIN_IO        2  /* Device memory */

/* DACR value: Domain 0 = Manager, Domain 1 = Client */
#define DACR_INIT        ((3 << (DOMAIN_KERNEL * 2)) | \
                          (1 << (DOMAIN_USER * 2)) | \
                          (1 << (DOMAIN_IO * 2)))
```

### 4.4 Execute Never (XN) Bit

- **Purpose**: Prevent instruction fetch from data pages
- **Security**: Mitigate code injection attacks
- **Usage**:
  - Set XN=1 for stack pages
  - Set XN=1 for heap pages
  - Set XN=0 for code pages
  - Set XN=1 for device memory

### 4.5 Shareable (S) Bit

- **S=0**: Non-shareable (private to CPU)
- **S=1**: Shareable (visible to other CPUs/DMA)
- **Usage**:
  - S=1 for SMP kernel memory
  - S=1 for DMA buffers
  - S=0 for per-CPU data

### 4.6 Global (nG) Bit

- **nG=0**: Global mapping (not flushed on ASID change)
- **nG=1**: Process-specific (flushed on context switch)
- **Usage**:
  - nG=0 for kernel mappings
  - nG=1 for user mappings

---

## 5. TLB Architecture

### 5.1 TLB Organization

```
ARM Cortex-A9 Example:
┌─────────────────────────────────────┐
│         Instruction TLB             │
│  - 32 entries (fully associative)   │
│  - 4KB, 64KB, 1MB, 16MB pages      │
│  - ASID tagged                      │
└─────────────────────────────────────┘

┌─────────────────────────────────────┐
│            Data TLB                 │
│  - 32 entries (fully associative)   │
│  - 4KB, 64KB, 1MB, 16MB pages      │
│  - ASID tagged                      │
└─────────────────────────────────────┘

┌─────────────────────────────────────┐
│         Unified L2 TLB              │
│  - 128 entries (4-way associative)  │
│  - Shared between I/D               │
└─────────────────────────────────────┘
```

### 5.2 TLB Entry Format

```
TLB Entry (Conceptual):
┌────────────────────────────────────────────────────────┐
│ ASID (8-bit) │ VA Tag │ PA │ Attributes │ Valid │ Size │
└────────────────────────────────────────────────────────┘

Attributes include:
- Access Permissions (AP, APX)
- Memory Type (TEX, C, B)
- Domain
- XN, S, nG bits
```

### 5.3 TLB Lookup Process

```
1. Virtual Address presented
2. Extract ASID from CONTEXTIDR
3. TLB lookup: Match (ASID, VA) pair
4. If HIT:
   - Check access permissions
   - Return physical address
   - Update access flags
5. If MISS:
   - Initiate page table walk
   - Load entry into TLB
   - Evict entry if TLB full (LRU/Random)
```

### 5.4 TLB Maintenance Operations

#### Invalidate Entire TLB
```assembly
; Invalidate entire unified TLB
MCR p15, 0, r0, c8, c7, 0

; Invalidate entire instruction TLB
MCR p15, 0, r0, c8, c5, 0

; Invalidate entire data TLB
MCR p15, 0, r0, c8, c6, 0
```

#### Invalidate by ASID
```assembly
; Invalidate unified TLB by ASID
MCR p15, 0, r0, c8, c7, 2  ; r0 = ASID
```

#### Invalidate by MVA (Modified Virtual Address)
```assembly
; Invalidate unified TLB entry by MVA
MCR p15, 0, r0, c8, c7, 1  ; r0 = MVA
```

#### Invalidate by MVA and ASID
```assembly
; Invalidate unified TLB entry by MVA, current ASID
MCR p15, 0, r0, c8, c7, 3  ; r0 = MVA
```

### 5.5 ASID (Address Space Identifier)

- **Size**: 8-bit (0-255)
- **Purpose**: Tag TLB entries with process ID
- **Benefit**: Avoid TLB flush on context switch
- **Register**: CONTEXTIDR[7:0]

```c
/* ASID allocation strategy */
#define ASID_BITS        8
#define NUM_ASIDS        (1 << ASID_BITS)  /* 256 */
#define ASID_MASK        (NUM_ASIDS - 1)

/* ASID 0 reserved for kernel */
#define ASID_KERNEL      0
#define ASID_FIRST_USER  1
```

---

## 6. System Control Registers

### 6.1 SCTLR (System Control Register)

```
Key bits for MMU:
[31]    Reserved
[30]    TE (Thumb Exception enable)
[29]    AFE (Access Flag Enable)
[28]    TRE (TEX Remap Enable)
[13]    V (Vectors bit)
[12]    I (Instruction cache enable)
[11]    Z (Branch prediction enable)
[2]     C (Data cache enable)
[1]     A (Alignment check enable)
[0]     M (MMU enable)
```

#### Critical Bits
```c
#define SCTLR_M   (1 << 0)   /* MMU enable */
#define SCTLR_A   (1 << 1)   /* Alignment check */
#define SCTLR_C   (1 << 2)   /* Data cache enable */
#define SCTLR_Z   (1 << 11)  /* Branch prediction */
#define SCTLR_I   (1 << 12)  /* Instruction cache */
#define SCTLR_V   (1 << 13)  /* High vectors (0xFFFF0000) */
#define SCTLR_TRE (1 << 28)  /* TEX remap enable */
#define SCTLR_AFE (1 << 29)  /* Access flag enable */
```

### 6.2 TTBCR (Translation Table Base Control Register)

```
[31]    EAE (Extended Address Enable) - LPAE
[5]     PD1 (TTBR1 page table walk disable)
[4]     PD0 (TTBR0 page table walk disable)
[2:0]   N (TTBR0/TTBR1 boundary)
```

### 6.3 CONTEXTIDR (Context ID Register)

```
[31:8]  PROCID (Process ID)
[7:0]   ASID (Address Space ID)
```

### 6.4 DFSR/IFSR (Data/Instruction Fault Status Register)

```
[12:10] FS[4:3] (Fault Status high bits)
[9]     Reserved
[8]     Reserved
[7:4]   Domain (Faulting domain)
[3:0]   FS[3:0] (Fault Status low bits)
```

#### Fault Status Codes
```
0b00001 = Alignment fault
0b00100 = Instruction cache maintenance fault
0b01100 = L1 translation, synchronous external abort
0b01110 = L2 translation, synchronous external abort
0b00101 = Translation fault, Section
0b00111 = Translation fault, Page
0b00011 = Access flag fault, Section
0b00110 = Access flag fault, Page
0b01001 = Domain fault, Section
0b01011 = Domain fault, Page
0b01101 = Permission fault, Section
0b01111 = Permission fault, Page
```

### 6.5 DFAR/IFAR (Data/Instruction Fault Address Register)

```
[31:0]  Faulting Virtual Address
```

---

## 7. Performance Considerations

### 7.1 TLB Miss Penalty

```
Typical Latencies (Cortex-A9):
- L1 TLB hit: 1 cycle
- L2 TLB hit: ~5 cycles
- L1 page table walk: ~20-30 cycles
- L2 page table walk: ~40-60 cycles
- Memory page table walk: ~100-200 cycles
```

### 7.2 Optimization Strategies

1. **Use Large Pages**: Reduce TLB pressure
   - 1MB sections for kernel image
   - 64KB pages for large buffers

2. **Minimize TLB Flushes**: 
   - Use ASID tagging
   - Selective invalidation

3. **Page Table Caching**:
   - Keep page tables in cacheable memory
   - Align page tables to cache line boundaries

4. **Reduce Page Table Depth**:
   - Use sections where possible
   - Avoid unnecessary L2 tables

### 7.3 Memory Ordering

```
Device Memory:
- Strongly-ordered: Strictest, no reordering
- Device: Allows some reordering
- Use for MMIO registers

Normal Memory:
- Cacheable: Best performance
- Non-cacheable: For DMA buffers
- Use barriers (DMB, DSB, ISB) for ordering
```

---

## 8. Security Considerations

### 8.1 Privilege Separation
- Use AP bits to enforce kernel/user separation
- Set XN on data pages
- Use domains for additional isolation

### 8.2 ASLR (Address Space Layout Randomization)
- Randomize user space mappings
- Randomize kernel module locations
- Requires flexible page table management

### 8.3 Guard Pages
- Unmapped pages between stack and heap
- Detect buffer overflows
- Use fault entries (descriptor[1:0] = 0b00)

---

## 9. References

- ARM Architecture Reference Manual ARMv7-A and ARMv7-R edition
- ARM Cortex-A Series Programmer's Guide
- Linux Kernel Documentation: ARM Memory Management
- ARM Generic Interrupt Controller Architecture Specification

---

**End of Document 1**
