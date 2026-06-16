# ARM32 Cache Architecture and DMA Coherency
## Document 5: Cache Hierarchy, Maintenance Operations, and DMA

**Author:** Senior Kernel Engineer  
**Target:** ARMv7-A (ARM32), Linux Kernel v5.x/v6.x  
**Scope:** L1/L2 cache micro-architecture, maintenance ops, DMA coherency, CCI  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview), Document 04 (TLB Management)

---

## Table of Contents
1. [ARM Cache Architecture Overview](#1-arm-cache-architecture-overview)
2. [Cache Indexing and Tagging Policies](#2-cache-indexing-and-tagging-policies)
3. [Cache Attributes in Page Descriptors](#3-cache-attributes-in-page-descriptors)
4. [Cache Maintenance Operations](#4-cache-maintenance-operations)
5. [L2 Cache Controller (PL310)](#5-l2-cache-controller-pl310)
6. [Shareability Domains and Memory Model](#6-shareability-domains-and-memory-model)
7. [DMA Coherency Problem and Solution](#7-dma-coherency-problem-and-solution)
8. [Linux DMA API](#8-linux-dma-api)
9. [ARM CCI (Cache Coherent Interconnect)](#9-arm-cci-cache-coherent-interconnect)
10. [Cache Alias Handling in Linux](#10-cache-alias-handling-in-linux)
11. [Performance and Profiling](#11-performance-and-profiling)
12. [Common Cache Bugs](#12-common-cache-bugs)

---

## 1. ARM Cache Architecture Overview

### 1.1 Typical ARMv7-A Cache Hierarchy

```
┌──────────────────────────────────────────────────────────────────┐
│                         CPU Core 0                               │
│                                                                   │
│  ┌─────────────┐    ┌─────────────┐                             │
│  │  I-Cache L1 │    │  D-Cache L1 │                             │
│  │  32KB, 4-way│    │  32KB, 4-way│ PIPT (Cortex-A9)           │
│  │  Line: 32B  │    │  Line: 32B  │ or VIPT (Cortex-A8)        │
│  └──────┬──────┘    └──────┬──────┘                             │
│         └──────────┬───────┘                                     │
│                    │                                              │
└────────────────────┼─────────────────────────────────────────────┘
                     │ L2 miss
                     ▼
┌──────────────────────────────────────────────────────────────────┐
│                    L2 Cache (Shared)                              │
│    ARM PL310 L2 Cache Controller                                 │
│    512KB–4MB, 8-way, line 32 bytes                               │
│    Unified (I+D), PIPT                                           │
│    Outer cache — between L1 and memory                           │
└────────────────────────────────────────────────────────────────  │
                     │ L2 miss
                     ▼
            ┌─────────────────┐
            │  System Memory  │  DDR3/LPDDR3
            │  (DRAM)         │
            └─────────────────┘
```

### 1.2 Cache Terminology

| Term | Definition |
|------|-----------|
| **Inner Cache** | Caches within the Inner Shareable domain (typically L1, L2 per cluster) |
| **Outer Cache** | L2/L3 caches outside Inner Shareable domain (e.g., system-level L3) |
| **Line Fill** | Fetching a full cache line (32 bytes typical) on miss |
| **Eviction** | Removing a cache line to make room; dirty lines written back |
| **Write-Through** | Every write goes to both cache and memory simultaneously |
| **Write-Back** | Writes go to cache only; memory updated on eviction |
| **Write-Allocate** | On write miss, allocate cache line, then write |
| **Read-Allocate** | On read miss, allocate cache line |
| **Clean** | Write dirty lines to next level (memory); keep in cache |
| **Invalidate** | Mark line(s) invalid; may lose dirty data if not cleaned first |
| **Flush** | Clean + Invalidate |

---

## 2. Cache Indexing and Tagging Policies

### 2.1 VIPT (Virtually Indexed, Physically Tagged) — Cortex-A8 L1

```
VIPT Lookup:

  Virtual Address (32-bit)
  ┌──────────────────┬───────────────────┬─────────────────┐
  │   VA Tag         │   Cache Index     │   Line Offset   │
  │   [31:13]        │   [12:5]          │   [4:0]         │
  └──────────────────┴───────────────────┴─────────────────┘
         ↓ (After TLB lookup)                   ↓
  Physical Tag [31:13]              Used directly for set selection
  
Process:
  1. Simultaneously send VA[12:5] to cache set selector
  2. Send VA to TLB for address translation
  3. Compare TLB-returned PA[31:13] with all 4 ways' tags
  
Advantage: Lookup starts before TLB completes (parallel)
Disadvantage: ALIASING possible
```

### 2.2 VIPT Aliasing Problem

```
Two VAs mapping same PA:
  VA1 = 0x00001000 → PA = 0xA0001000, VA1[12:5] = 0x00
  VA2 = 0x00003000 → PA = 0xA0001000, VA2[12:5] = 0x18

Different cache sets selected → TWO cache lines for same PA!
  Write to VA1 → updates set 0x00
  Read  from VA2 → stale data from set 0x18

Condition for aliasing: VA[12:PAGE_SHIFT] ≠ PA[12:PAGE_SHIFT]
On Cortex-A8: Page Coloring required for shared pages

Linux fix: mmap user pages such that VA[12:PAGE_SHIFT] = PA[12:PAGE_SHIFT]
           (page coloring in arch_get_unmapped_area)
           For non-conforming: flush_cache_page() before mapping
```

### 2.3 PIPT (Physically Indexed, Physically Tagged) — Cortex-A9+ L1

```
PIPT Lookup:

  Physical Address (after TLB)
  ┌──────────────────┬───────────────────┬─────────────────┐
  │   PA Tag         │   Cache Index     │   Line Offset   │
  │   [31:13]        │   [12:5]          │   [4:0]         │
  └──────────────────┴───────────────────┴─────────────────┘

Process:
  1. TLB lookup: VA → PA (must complete first)
  2. Use PA[12:5] to select cache set
  3. Compare PA[31:13] with way tags

Advantage: No aliasing (one PA → exactly one cache location)
Disadvantage: Can't start lookup before TLB returns
              (critical path: TLB + cache in series)

Cortex-A9 optimization: L1 cache is virtually banked to hide TLB latency
```

### 2.4 Cache Geometry Discovery

```assembly
/* CTR (Cache Type Register, CP15 c0 c0 1) */
MRC p15, 0, r0, c0, c0, 1

/* CCSIDR (Cache Size ID Register, CP15 c0 c0 0) — select first */
MOV r0, #0              @ select L1 D-cache
MCR p15, 2, r0, c0, c0, 0   @ write CSSELR (Cache Size Selection)
ISB
MRC p15, 1, r0, c0, c0, 0   @ read CCSIDR

/* CCSIDR format:
 * [27:13] NumSets-1      (number of sets - 1)
 * [12:3]  Associativity-1 (ways - 1)
 * [2:0]   LineSize        (log2(bytes/line) - 2)
 */

/* Compute set/way for full cache flush */
line_size   = 1 << ((ccsidr & 0x7) + 2 + 2)   @ bytes per line
ways        = ((ccsidr >> 3) & 0x3FF) + 1
sets        = ((ccsidr >> 13) & 0x7FFF) + 1
```

---

## 3. Cache Attributes in Page Descriptors

### 3.1 TEX/C/B Encoding for Cache Policy

```
Section/Page descriptor bits: TEX[2:0], C, B

Without TEX remap (SCTLR.TRE=0):
┌──────┬───┬───┬─────────────────────────────────────────────────┐
│ TEX  │ C │ B │ Memory Type & Cache Policy                       │
├──────┼───┼───┼─────────────────────────────────────────────────┤
│ 000  │ 0 │ 0 │ Strongly-Ordered                                │
│ 000  │ 0 │ 1 │ Shared Device                                   │
│ 000  │ 1 │ 0 │ Normal: Outer+Inner Write-Through, no Write-Alloc│
│ 000  │ 1 │ 1 │ Normal: Outer+Inner Write-Back, no Write-Alloc  │
│ 001  │ 0 │ 0 │ Normal: Outer+Inner Non-Cacheable               │
│ 001  │ 0 │ 1 │ Implementation defined                           │
│ 001  │ 1 │ 0 │ Normal: Outer Non-Cacheable, Inner WT           │
│ 001  │ 1 │ 1 │ Normal: Outer Non-Cacheable, Inner WB+WA        │
│ 010  │ 0 │ 0 │ Non-Shared Device                               │
│ 100  │ 0 │ 0 │ Normal: Outer+Inner Non-Cacheable               │
│ 101  │ 0 │ 0 │ Normal: Outer WB+WA, Inner Non-Cacheable        │
│ 110  │ 0 │ 0 │ Normal: Outer WT+RA, Inner Non-Cacheable        │
│ 111  │ 0 │ 0 │ Normal: Outer WB+WA+RA, Inner Non-Cacheable     │
│ 100  │ 1 │ 1 │ Normal: Outer+Inner WB+WA (most common, perf)   │
└──────┴───┴───┴─────────────────────────────────────────────────┘
```

### 3.2 Linux ARM Memory Type Macros

```c
/* arch/arm/include/asm/pgtable-2level.h */
/* arch/arm/include/asm/mmu.h */

/* Page table protection types */
#define MT_DEVICE                   0   /* Strongly-ordered, uncached */
#define MT_DEVICE_NONSHARED         1   /* Device, non-shared */
#define MT_DEVICE_CACHED            2   /* Device, cached (avoid if possible) */
#define MT_DEVICE_WC                3   /* Write-combining (Normal NC) */
#define MT_UNCACHED                 4   /* Normal, non-cacheable */
#define MT_CACHECLEAN               5   /* Normal, clean write-through */
#define MT_MINICLEAN                6   /* Normal, mini-clean */
#define MT_LOW_VECTORS              7   /* 0x00000000 - exception vectors */
#define MT_HIGH_VECTORS             8   /* 0xFFFF0000 - exception vectors */
#define MT_MEMORY_RWX               9   /* Normal, RWX, cacheable, bufferable */
#define MT_MEMORY_RW               10   /* Normal, RW, no execute */
#define MT_ROM                     11   /* Read-only, cached */
#define MT_MEMORY_RWX_NONCACHED    12   /* Normal, RWX, non-cached */
#define MT_MEMORY_RW_DTCM          13   /* Data TCM */
#define MT_MEMORY_RWX_ITCM         14   /* Instruction TCM */
#define MT_MEMORY_RW_SO            15   /* Normal, RW, strongly-ordered */
#define MT_MEMORY_DMA_READY        16   /* Normal, NC — DMA buffer */
```

### 3.3 PRRR/NMRR — TEX Remap (SCTLR.TRE=1)

```
With TEX remapping enabled (default in many Linux configs):
TEX[2:0] and C, B become region indices [2:0] (0–7).

PRRR (Primary Region Remap Register, CP15 c10 c2 0):
  8 regions × 4 bits = 32 bits
  Bits [2n+1:2n] for region n:
    0b00 = Strongly-ordered
    0b01 = Device
    0b10 = Normal
    0b11 = Reserved

NMRR (Normal Memory Remap Register, CP15 c10 c2 1):
  8 regions × 4 bits = 32 bits for outer, 32 bits for inner
  Inner cache: bits [2n+1:2n]
  Outer cache: bits [2n+17:2n+16]
    0b00 = Non-cacheable
    0b01 = Write-Back, Write-Allocate
    0b10 = Write-Through, no Write-Allocate
    0b11 = Write-Back, no Write-Allocate

Linux default setup (arch/arm/mm/mmu.c):
  Region 0: Strongly-Ordered      (MMIO devices)
  Region 1: Shared Device          (device memory)
  Region 4: Normal, WB+WA inner   (kernel RAM)
  Region 8: Normal, NC            (DMA coherent buffers)
```

---

## 4. Cache Maintenance Operations

### 4.1 L1 Cache Operations by MVA (Virtual Address)

```assembly
/* Clean D-cache line by MVA to PoC (Point of Coherency) */
/* PoC: typically system memory — ensures DMA can see the data */
/* r0 = VA of cache line to clean */
MCR p15, 0, r0, c7, c10, 1  @ DCCMVAC (Data Cache Clean by MVA to PoC)

/* Clean D-cache line to PoU (Point of Unification) */
/* PoU: L2 cache — ensures I-cache and D-cache are coherent */
/* Used before I-cache invalidate (JIT compiler, self-modifying code) */
MCR p15, 0, r0, c7, c11, 1  @ DCCMVAU (Data Cache Clean by MVA to PoU)

/* Invalidate D-cache line by MVA to PoC */
/* WARNING: loses dirty data — use only for DMA-IN (device writes to RAM) */
MCR p15, 0, r0, c7, c6, 1   @ DCIMVAC (Data Cache Invalidate by MVA to PoC)

/* Clean and Invalidate D-cache line by MVA to PoC */
/* Most common for DMA-OUT (CPU writes, device reads) */
MCR p15, 0, r0, c7, c14, 1  @ DCCIMVAC

/* Invalidate I-cache line by MVA to PoU */
MCR p15, 0, r0, c7, c5, 1   @ ICIMVAU

/* Invalidate entire I-cache to PoU */
MCR p15, 0, r0, c7, c5, 0   @ ICIALLU
```

### 4.2 L1 Cache Operations by Set/Way (Full Cache Flush)

```assembly
/*
 * Full D-cache flush (clean + invalidate) — used at shutdown, suspend
 * Must iterate all sets and ways since no "flush all" single instruction
 */

/* Read cache geometry */
MRC p15, 1, r0, c0, c0, 0   @ CCSIDR
AND r1, r0, #7               @ r1 = line size field
ADD r1, r1, #4               @ r1 = log2(line_size_bytes)
                              @ e.g., field=1 → 1+4=5 → 32 bytes
UBFX r2, r0, #3, #10         @ r2 = associativity - 1
UBFX r3, r0, #13, #15        @ r3 = num_sets - 1

/* Compute bit positions */
CLZ r4, r2                   @ r4 = 32 - log2(ways) = way shift

/* Outer loop: for each set */
MOV r5, r3
outer_loop:
    MOV r6, r2
    /* Inner loop: for each way */
    inner_loop:
        ORR r7, r5, r6, LSL r4  @ r7 = set | (way << way_shift)
        LSL r7, r7, r1           @ r7 <<= line_size_shift
        MCR p15, 0, r7, c7, c14, 2  @ DCCISW — Clean+Inv by Set/Way
        SUBS r6, r6, #1
        BGE inner_loop
    SUBS r5, r5, #1
    BGE outer_loop

DSB                          @ Ensure all clean ops complete
```

### 4.3 Point of Coherency (PoC) vs Point of Unification (PoU)

```
Memory hierarchy and coherency points:

CPU ─ L1 I-Cache ─┐
                   ├─ PoU (Point of Unification)
CPU ─ L1 D-Cache ─┘    (typically after L2 cache, or end of L1 on UP)
         │
         ▼
     L2 Cache ────────── PoC (Point of Coherency)
         │                   (typically system memory / last cache before DRAM)
         ▼                   (where DMA engines and other bus masters see data)
       DRAM

PoU operations: flush L1 D → L1 I coherent (for self-modifying code / JIT)
  DCCMVAU + ICIMVAU sequence

PoC operations: flush L1 + L2 → DRAM visible (for DMA)
  DCCMVAC + (L2 maintenance if needed)
```

### 4.4 Linux Cache Flush API

```c
/* arch/arm/include/asm/cacheflush.h */

/* Flush (clean+invalidate) D-cache range for DMA-OUT */
void flush_dcache_range(unsigned long start, unsigned long end);
    /* → DCCMVAC per line, DSB */

/* Invalidate D-cache range for DMA-IN (device wrote to RAM) */
void invalidate_dcache_range(unsigned long start, unsigned long end);
    /* → DCIMVAC per line */
    /* CRITICAL: start and end must be cache-line aligned! */
    /*           Otherwise clean first to avoid losing dirty data */

/* Flush entire I-cache */
void flush_icache_all(void);
    /* → ICIALLU, DSB, ISB */

/* Flush I-cache range (after writing new code) */
void flush_icache_range(unsigned long start, unsigned long end);
    /* → DCCMVAU + ICIMVAU per line, DSB, ISB */

/* Flush cache page (for page coloring / COW) */
void flush_cache_page(struct vm_area_struct *vma, unsigned long addr,
                       unsigned long pfn);

/* Flush all caches for an mm (context switch on VIVT) */
void flush_cache_mm(struct mm_struct *mm);
```

### 4.5 Cache Maintenance Before/After DMA

```
DMA-OUT: CPU prepares buffer, DMA device reads it
  CPU wrote to buffer → dirty cache lines exist
  DMA reads PA directly (bypasses CPU cache)
  → DMA sees STALE data if cache not flushed

  Required:
    [1] CPU writes data to buffer (VA)
    [2] dma_map_single() → flush_dcache_range() → DCCMVAC
    [3] DMA reads PA: sees fresh data ✓

DMA-IN: DMA device writes to buffer, CPU reads it
  DMA writes to PA
  CPU reads VA → cache HIT on STALE line (predates DMA write)
  → CPU sees old data

  Required:
    [1] dma_map_single() (direction=DMA_FROM_DEVICE)
         → invalidate_dcache_range() BEFORE DMA starts
    [2] DMA writes to PA: goes straight to DRAM
    [3] dma_unmap_single() → invalidate_dcache_range() AGAIN
    [4] CPU reads VA: cache MISS → fresh data from DRAM ✓
```

---

## 5. L2 Cache Controller (PL310)

### 5.1 PL310 Architecture

```
ARM PL310 (Level-2 Cache Controller) — widely used in ARMv7-A SoCs:
  (Qualcomm MSM8960, Samsung Exynos, TI OMAP)

┌──────────────────────────────────────────────────────────┐
│                    ARM PL310 L2 Cache                    │
│                                                           │
│  Size: 128KB–4MB (configurable at synthesis)             │
│  Associativity: 4-way, 8-way, or 16-way                  │
│  Line Size: 32 bytes (256 bit AXI bus)                   │
│  Tag RAM latency: 1–3 cycles                             │
│  Data RAM latency: 1–3 cycles                            │
│                                                           │
│  Registers base: typically 0xF0000000 (SoC dependent)   │
│  ┌──────────────────────────────────────────┐            │
│  │ reg0_cache_id    (0x000) Read-only       │            │
│  │ reg1_control     (0x100) Enable bit      │            │
│  │ reg1_aux_control (0x104) Config          │            │
│  │ reg7_cache_sync  (0x730) Sync (barrier)  │            │
│  │ reg7_inv_way     (0x77C) Invalidate all  │            │
│  │ reg7_clean_way   (0x7BC) Clean all ways  │            │
│  │ reg7_clean_inv   (0x7FC) Clean+Inv all   │            │
│  │ reg9_d_lockdown  (0x900) Lockdown        │            │
│  └──────────────────────────────────────────┘            │
└──────────────────────────────────────────────────────────┘
```

### 5.2 PL310 Enable and Configuration

```c
/* arch/arm/mm/cache-l2x0.c */

void l2x0_init(void __iomem *base, u32 aux_val, u32 aux_mask)
{
    u32 aux;
    u32 cache_id;
    u32 way_size;

    cache_id = readl_relaxed(base + L2X0_CACHE_ID);

    /* 1. Disable L2 cache before configuring */
    l2x0_disable();   /* reg1_control bit[0] = 0 */

    /* 2. Configure auxiliary control:
     *   [16]  Associativity (0=8-way, 1=16-way)
     *   [19:17] Way-size (1=16KB, 2=32KB, 3=64KB, 4=128KB per way)
     *   [22]  Shared attribute override enable
     *   [25]  Round-robin replacement
     *   [28]  Data prefetch enable
     *   [29]  Instruction prefetch enable
     *   [30]  Early BRESP enable (AXI)
     */
    aux = readl_relaxed(base + L2X0_AUX_CTRL);
    aux &= aux_mask;
    aux |= aux_val;
    writel_relaxed(aux, base + L2X0_AUX_CTRL);

    /* 3. Invalidate entire L2 before enabling */
    l2x0_inv_all();   /* reg7_inv_way: write all way bits, poll until 0 */

    /* 4. Enable L2 */
    writel_relaxed(1, base + L2X0_CTRL);  /* reg1_control bit[0] = 1 */
}
```

### 5.3 PL310 Secure Register Access (TrustZone)

```
PL310 registers are banked: Secure vs Non-secure access.

Secure registers (accessible only in Secure PL1):
  reg1_control    (0x100) — L2 enable/disable
  reg1_aux_control (0x104) — Cache configuration
  reg9_d_lockdown (0x900) — Lockdown by way

Non-secure registers (accessible from HLOS):
  reg7_cache_sync   — sync
  reg7_clean_by_pa  — clean by PA
  reg7_inv_by_pa    — invalidate by PA
  reg7_clean_inv_pa — clean+inv by PA
  reg15_debug_ctrl  — debug

Implication for Linux: On TrustZone systems (Qualcomm, most modern SoCs),
the secure firmware (TF-A BL31 or QSEE) owns PL310 control registers.
Linux calls firmware via secure monitor call (SMC) or OUTER_CACHE ops.
```

### 5.4 Outer Cache API in Linux

```c
/* arch/arm/include/asm/outercache.h */

struct outer_cache_fns {
    void (*inv_range)(unsigned long, unsigned long);
    void (*clean_range)(unsigned long, unsigned long);
    void (*flush_range)(unsigned long, unsigned long);
    void (*flush_all)(void);
    void (*disable)(void);
    void (*set_debug)(unsigned long);
    void (*sync)(void);
    void (*resume)(void);
};

extern struct outer_cache_fns outer_cache;

/* Usage: */
outer_cache.flush_range(pa_start, pa_end);  /* Clean + Inv L2 by PA */
outer_cache.inv_range(pa_start, pa_end);    /* Invalidate L2 by PA */
outer_cache.clean_range(pa_start, pa_end);  /* Clean (writeback) L2 by PA */

/* Automatically called by DMA API (dma_map_single etc.) */
```

---

## 6. Shareability Domains and Memory Model

### 6.1 Shareability Domains

```
ARM defines three shareability domains:

┌──────────────────────────────────────────────────────────────────┐
│                    Outer Shareable Domain                        │
│  (All CPU clusters, GPU, DMA engines, other bus masters)        │
│                                                                   │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              Inner Shareable Domain (Cluster 0)            │ │
│  │                                                             │ │
│  │  ┌────────────────┐  ┌────────────────┐                   │ │
│  │  │   CPU Core 0   │  │   CPU Core 1   │                   │ │
│  │  │  (Non-Shareable│  │  (Non-Shareable│                   │ │
│  │  │    domain)     │  │    domain)     │                   │ │
│  │  └────────────────┘  └────────────────┘                   │ │
│  │                   L2 Cache                                  │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              Inner Shareable Domain (GPU)                  │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 6.2 Shareable Attribute Impact

```
Page Descriptor S bit and TEX encoding controls shareability:

S=0 (Non-Shareable):
  - Memory accesses private to this CPU
  - No coherency hardware engaged
  - Faster (no snoop traffic on bus)
  - Use for: CPU-private data structures

S=1 (Inner Shareable):
  - Hardware cache coherency within Inner Shareable domain
  - Cortex-A9 MPCore: snoops other cores' L1 caches
  - Use for: SMP kernel data, mutex, spinlock, shared user memory

Outer Shareable (encoded in TEX for Normal memory):
  - Hardware coherency extends to outer shareable domain
  - Required for: data shared between CPU cluster and GPU/DMA
  - Cost: higher bus traffic

Strongly-Ordered and Device: always treated as Outer Shareable
```

### 6.3 Shareable Domain and DMB/DSB Options

```assembly
/* DSB/DMB can be scoped to a shareability domain */

DSB         @ Full system (same as DSB SY)
DSB SY      @ All memory accesses, full system
DSB ISH     @ All accesses, Inner Shareable domain only
DSB NSH     @ All accesses, Non-Shareable domain only
DSB OSH     @ All accesses, Outer Shareable domain only

DMB ISH     @ Ensures ordering within Inner Shareable domain
            @ Sufficient for SMP synchronization between CPU cores
            @ Cheaper than DMB SY (doesn't wait for DMA engines)

DMB OSH     @ Required when synchronizing with GPU/DMA
            @ More expensive than DMB ISH
```

---

## 7. DMA Coherency Problem and Solution

### 7.1 Hardware Coherent DMA vs Software Coherent DMA

```
Hardware Coherent DMA:
  - DMA engine is integrated into Inner Shareable domain
  - Snoops CPU caches automatically
  - CPU and DMA always see same data
  - No software cache maintenance required
  - Example: ARM ACP (Accelerator Coherency Port) connected DMA
  - Example: Most modern SoC DMA engines on ARM Cortex-A15+

  dma_alloc_coherent() → just allocates uncached memory
                          (still non-cacheable to avoid coherency overhead)

Software Coherent DMA (most ARM32 SoCs):
  - DMA engine NOT in Inner Shareable domain
  - DMA sees physical memory directly (bypasses CPU caches)
  - CPU cache and DRAM can be out of sync
  - Software must explicitly manage coherency
  - Example: USB DMA, SD DMA on Qualcomm MSM8960
```

### 7.2 DMA Coherent Buffer (dma_alloc_coherent)

```
┌────────────────────────────────────────────────────────────────┐
│             DMA Coherent Buffer                                 │
│                                                                 │
│  Physical page(s): mapped UNCACHED in kernel VA space          │
│  (TEX=000, C=0, B=0 = Strongly-Ordered, or MT_UNCACHED)       │
│                                                                 │
│  CPU accesses: bypass L1/L2 cache → go directly to DRAM       │
│  DMA reads:    from DRAM                                        │
│  DMA writes:   to DRAM                                         │
│                                                                 │
│  Always consistent: CPU + DMA both see DRAM                    │
│  Penalty: CPU reads/writes SLOW (no caching)                   │
└────────────────────────────────────────────────────────────────┘
```

```c
/* Allocate a coherent buffer */
void *cpu_addr = dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL);
/* cpu_addr: CPU virtual address (uncached) */
/* dma_handle: physical/IOVA address for device */

/* Use:
 *   CPU writes: cpu_addr[i] = value;   ← writes go directly to DRAM
 *   DMA reads: dma_handle               ← device reads same DRAM
 */

dma_free_coherent(dev, size, cpu_addr, dma_handle);
```

### 7.3 Streaming DMA (dma_map_single) — Cached Buffer

```c
/*
 * Use when: Buffer is used for one DMA transfer, then reclaimed by CPU
 * Allows buffer to be cached (faster CPU access)
 * Software handles cache maintenance at map/unmap
 */

/* DMA-OUT: CPU writes buffer, DMA reads it */
dma_addr_t dma_handle = dma_map_single(dev, cpu_ptr, size, DMA_TO_DEVICE);
/*
 * DMA_TO_DEVICE: performs:
 *   flush_dcache_range(cpu_ptr, cpu_ptr+size)   ← writeback dirty cache
 *   outer_cache.clean_range(pa, pa+size)         ← clean L2 cache
 * DMA engine now reads clean data from DRAM
 */

/* Initiate DMA transfer */
setup_dma_descriptor(dma_handle, size);
trigger_dma();
wait_for_dma_complete();

/* Unmap: allow CPU to access buffer again */
dma_unmap_single(dev, dma_handle, size, DMA_TO_DEVICE);
/* DMA_TO_DEVICE unmap: no additional flush needed (CPU is reading) */

/* DMA-IN: DMA writes buffer, CPU reads it */
dma_addr_t dma_handle = dma_map_single(dev, cpu_ptr, size, DMA_FROM_DEVICE);
/*
 * DMA_FROM_DEVICE: performs:
 *   invalidate_dcache_range(cpu_ptr, cpu_ptr+size) ← discard stale L1
 *   outer_cache.inv_range(pa, pa+size)              ← discard stale L2
 *   CRITICAL: cpu_ptr must be cache-line aligned!
 *             Misaligned → partial line invalidate → data loss!
 */

trigger_dma_receive();
wait_for_dma_complete();

dma_unmap_single(dev, dma_handle, size, DMA_FROM_DEVICE);
/*
 * DMA_FROM_DEVICE unmap: invalidate cache again
 *   → CPU reads miss cache → fresh DMA data from DRAM
 */
```

### 7.4 DMA Mapping Rules (Critical for interviews)

```
Rule 1: Cache-line Alignment
  DMA buffers MUST be cache-line aligned at both start and end.
  Partial cache-line invalidate can corrupt adjacent data.
  
  void *buf = kmalloc(size, GFP_KERNEL | __GFP_DMA);
  BUG_ON((unsigned long)buf & (L1_CACHE_BYTES - 1));

Rule 2: No CPU Access Between map and unmap
  Between dma_map_single() and dma_unmap_single():
    - CPU must NOT read/write the buffer
    - Buffer "owned" by DMA engine
    - Violating this = undefined behavior (may see stale data)

Rule 3: Use dma_sync_* for Ping-Pong Buffers
  If CPU and DMA alternately own buffer:
  dma_sync_single_for_cpu(dev, handle, size, dir)    ← give to CPU
  dma_sync_single_for_device(dev, handle, size, dir) ← give to DMA

Rule 4: DMA Mask
  dev->dma_mask limits physical addresses DMA can access
  ARM32: typical DMA_BIT_MASK(32) → 4GB PA space
  Some peripherals: DMA_BIT_MASK(30) → 1GB PA limit → need bounce buffers
```

---

## 8. Linux DMA API

### 8.1 DMA API Call Flow

```
dma_map_single(dev, va, size, dir)
  │
  ├─ [if dev has IOMMU/SMMU attached]
  │    → iommu_map() → create IOVA → SMMU maps IOVA→PA
  │    → Returns IOVA (not PA)
  │
  └─ [if no IOMMU, direct DMA]
       → swiotlb_map() or direct PA
       → Perform cache maintenance (ARM-specific):
           arch_sync_dma_for_device()
             → flush_dcache_range() for L1
             → outer_cache.flush_range() for L2
       → Returns PA (physical address)
```

### 8.2 DMA Pools

```c
/*
 * DMA Pool: Pre-allocated coherent memory pool for small buffers
 * Avoids repeated dma_alloc_coherent() overhead
 * Used by: USB host controllers, I2C DMA, SPI DMA
 */

struct dma_pool *pool = dma_pool_create("my_pool", dev,
                                         allocation_size,  /* each block size */
                                         alignment,        /* power of 2 */
                                         boundary);        /* don't cross this */

void *cpu_addr;
dma_addr_t handle;
cpu_addr = dma_pool_alloc(pool, GFP_KERNEL, &handle);

/* Use buffer */
cpu_addr[0] = 0xDEAD;

dma_pool_free(pool, cpu_addr, handle);
dma_pool_destroy(pool);
```

### 8.3 Scatter-Gather DMA

```c
/*
 * Scatter-Gather: DMA from/to physically discontiguous pages
 * Each sg entry = one physically contiguous segment
 */

struct scatterlist sg[3];
sg_init_table(sg, 3);
sg_set_page(&sg[0], page0, PAGE_SIZE, 0);
sg_set_page(&sg[1], page1, PAGE_SIZE, 0);
sg_set_page(&sg[2], page2, PAGE_SIZE, 0);

int nents = dma_map_sg(dev, sg, 3, DMA_TO_DEVICE);
/* Returns number of hardware SG entries (may merge adjacent PAs) */
/* Also performs cache flush on each segment */

for_each_sg(sg, s, nents, i) {
    setup_hw_sg_entry(sg_dma_address(s), sg_dma_len(s));
}
trigger_sg_dma();
wait_complete();

dma_unmap_sg(dev, sg, 3, DMA_TO_DEVICE);
```

---

## 9. ARM CCI (Cache Coherent Interconnect)

### 9.1 CCI-400 Architecture (ARM Cortex-A15 / big.LITTLE)

```
┌──────────────────────────────────────────────────────────────────┐
│                       CCI-400                                    │
│               Cache Coherent Interconnect                        │
│                                                                   │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐                 │
│  │  Master 0  │  │  Master 1  │  │  Master 2  │  (CPU clusters) │
│  │ Cluster A15│  │ Cluster A7 │  │    GPU     │                 │
│  └────────────┘  └────────────┘  └────────────┘                 │
│                        │                                          │
│           ┌────────────┴────────────┐                           │
│           │      Snoop Control Unit │                           │
│           │   (ACE protocol: AXI + snoop channels)             │
│           └────────────┬────────────┘                           │
│                        │                                          │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐                 │
│  │  Slave 0   │  │  Slave 1   │  │  Slave 2   │  (peripherals) │
│  │    DRAM    │  │  Periph    │  │    DMA     │                 │
│  └────────────┘  └────────────┘  └────────────┘                 │
└──────────────────────────────────────────────────────────────────┘

ACE (AXI Coherency Extensions):
  - Snoop requests: ReadClean, ReadShared, CleanInvalid
  - Automatically maintains cache coherency between clusters
  - big.LITTLE: A15 and A7 clusters coherent without software intervention
```

### 9.2 CCI-400 Barrier Registers

```c
/* CCI-400 provides a barrier register for coherent DMA synchronization */

/* Wait for all coherency transactions to complete */
void cci_enable_snoop_dvm_reqs(unsigned int master_id)
{
    struct cci_nb_ports const *nb_ports = &cci_config_table[master_id];
    unsigned int port = nb_ports->port;

    /* Enable snoop and DVM requests from this master */
    writel_relaxed(readl_relaxed(cci_ctrl_base + SNOOP_CTRL_REG(port))
                   | (1 << 0) | (1 << 1),
                   cci_ctrl_base + SNOOP_CTRL_REG(port));

    /* Poll status register until change committed */
    while (readl_relaxed(cci_ctrl_base + STATUS_REG) & STATUS_CHANGE_PENDING)
        ;
}
```

---

## 10. Cache Alias Handling in Linux

### 10.1 ARM32 VIPT Alias Handling in mmap

```c
/* arch/arm/mm/mmap.c */
/*
 * For VIPT caches, ensure VA page color matches PA page color.
 * Color = page_offset[CACHE_ALIAS_BITS:PAGE_SHIFT]
 *
 * If VA and PA colors differ → cache aliasing possible
 * → Force VA alignment so VA[12:PAGE_SHIFT] == PA[12:PAGE_SHIFT]
 */

unsigned long arch_get_unmapped_area(struct file *filp,
    unsigned long addr, unsigned long len,
    unsigned long pgoff, unsigned long flags)
{
    if (cache_is_vipt_aliasing()) {
        /* For file-backed mmaps: align VA to match PA color */
        if (filp) {
            pgoff = filp->f_mapping->host->i_ino;  /* deterministic color */
            addr = COLOUR_ALIGN(addr, pgoff);
        }
    }
    /* ... find free VMA region ... */
}

#define CACHE_COLOUR(vaddr) ((vaddr & (SHMLBA-1)) >> PAGE_SHIFT)
```

### 10.2 Flush on COW for VIPT

```c
/* When breaking a COW page on a VIPT cache system: */
static vm_fault_t do_wp_page(struct vm_fault *vmf)
{
    /* ... */
    if (cache_is_vipt_aliasing()) {
        /* Flush old page from cache before remap */
        flush_cache_page(vma, vmf->address, pfn);
    }
    /* Remap with new physical page */
    set_pte_at(mm, vmf->address, vmf->pte, new_entry);

    if (cache_is_vipt_aliasing()) {
        /* Flush new mapping to ensure correct color */
        flush_cache_page(vma, vmf->address, new_pfn);
    }
}
```

---

## 11. Performance and Profiling

### 11.1 Cache Miss Rate via PMU

```bash
# ARM PMU events (Cortex-A9):
# 0x03: L1 D-cache refill (L1 miss → L2 fill)
# 0x04: L1 D-cache access
# 0x19: L2 cache access
# 0x1A: L2 cache refill (L2 miss → DRAM access)

perf stat -e L1-dcache-load-misses,L1-dcache-loads,\
             LLC-load-misses,LLC-loads ./workload

# Expected ratios for well-optimized kernel code:
# L1 miss rate: < 5%
# L2 miss rate: < 1%
```

### 11.2 Cache-Friendly Data Structures

```c
/* Cache line size = 32 bytes (typical ARM32) */
#define CACHE_LINE_SIZE 32

/* Align frequently-accessed struct to cache line */
struct hot_data {
    spinlock_t lock;
    unsigned long counter;
    void *ptr;
} __attribute__((aligned(CACHE_LINE_SIZE)));

/* Separate hot/cold data to avoid false sharing in SMP */
struct per_cpu_data {
    /* Hot: frequently accessed */
    unsigned long hit_count;       /* offset 0  */
    unsigned long miss_count;      /* offset 4  */
    /* Cold: rarely accessed — put in separate cache line */
    unsigned long __attribute__((aligned(CACHE_LINE_SIZE)))
                 debug_info;
};
```

---

## 12. Common Cache Bugs

### 12.1 Partially Aligned DMA Buffer (Critical)

```c
/* BUG: DMA buffer not cache-line aligned at end */
struct my_dma_header {
    u32 command;
    u32 length;
    /* total = 8 bytes, but cache line = 32 bytes */
};

struct my_data {
    struct my_dma_header hdr;    /* offset 0, 8 bytes */
    u32 other_data;              /* offset 8, 4 bytes — SHARES cache line with hdr! */
    u8 dma_buffer[256];          /* offset 12, 256 bytes */
};

/* BUG: When invalidating dma_buffer for DMA-IN:
 *   invalidate_dcache_range(dma_buffer, dma_buffer+256)
 *   But dma_buffer[0..19] shares a cache line with other_data!
 *   Invalidate discards that cache line → other_data reads stale value!
 */

/* FIX: Ensure DMA buffer is cache-line aligned */
struct my_data {
    struct my_dma_header hdr;
    u32 other_data;
    u8 __attribute__((aligned(L1_CACHE_BYTES))) dma_buffer[256];
};
```

### 12.2 Self-Modifying Code Without I-Cache Flush

```c
/* JIT compiler writes new instructions to memory */
void jit_compile(void *code_ptr, size_t code_size) {
    /* Write machine code */
    memcpy(code_ptr, generated_opcodes, code_size);

    /* BUG: No cache maintenance! I-cache has old data */
    /* Execute code_ptr → may execute stale I-cache content */

    /* FIX: */
    flush_icache_range((unsigned long)code_ptr,
                        (unsigned long)code_ptr + code_size);
    /* Steps:
     *   1. DCCMVAU: clean D-cache to PoU (L2)
     *   2. DSB ISH: wait for clean
     *   3. ICIMVAU: invalidate I-cache to PoU
     *   4. ISB: flush pipeline
     */
}
```

### 12.3 spectre-BTB and Cache Side-Channel Awareness

```
Cache side-channel attacks (relevant for Google/Qualcomm interviews):

Flush+Reload: Attacker and victim share page (shared library).
  1. Attacker: flush_dcache_line(target_address)
  2. Victim: accesses target_address (cache loads line)
  3. Attacker: time load of target_address
     - Fast → cache hit → victim accessed this line
     - Slow → cache miss → victim did NOT access

Linux mitigations on ARM32:
  - CONFIG_RANDOMIZE_BASE: KASLR (kernel ASLR)
  - CONFIG_HARDEN_BRANCH_PREDICTOR: flush BTB on kernel entry
  - flush_icache_all() on return to user (for some Spectre variants)
  - CONFIG_UNMAP_KERNEL_AT_EL0 (KPTI): not standard on ARM32
```

---

## Summary

| Operation | L1 Cache | L2 Cache (PL310) | When |
|-----------|----------|------------------|------|
| DMA-OUT flush | `DCCMVAC` by VA | `outer_cache.clean_range` by PA | Before DMA reads |
| DMA-IN invalidate | `DCIMVAC` by VA | `outer_cache.inv_range` by PA | Before+After DMA writes |
| JIT / exec code | `DCCMVAU` + `ICIMVAU` | Not needed (PoU) | After writing code |
| Suspend/resume | Set/Way clean+inv | `outer_cache.flush_all` | Before power off |
| Cache disable | Set/Way clean+inv | Disable via reg1_control | Before SCTLR.C=0 |

---

**Cross-References:**
- Doc 01: Memory attribute encoding (TEX/C/B)
- Doc 04: TLB and cache ordering, DSB requirements
- Doc 07: SMMU cache maintenance for IOMMU-mapped DMA
- Doc 08: DMB/DSB scope and usage with cache operations

---
**End of Document 5**
