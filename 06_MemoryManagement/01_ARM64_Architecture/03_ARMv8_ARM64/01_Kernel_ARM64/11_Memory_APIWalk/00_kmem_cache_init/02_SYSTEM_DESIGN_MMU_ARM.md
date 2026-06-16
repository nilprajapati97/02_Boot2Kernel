# `kmem_cache_init` — System Design: MMU & Memory Architecture on ARM32 and ARM64

> **Focus**: How the SLUB slab allocator interacts with ARM32 and ARM64 Memory Management Units, cache architecture, DMA zones, NUMA, and memory tagging.  
> **Level**: Nvidia Senior Kernel Engineer / Silicon Memory Architect Interview  
> **Prerequisite**: Familiarity with `01_DESIGN_CODE_WALKTHROUGH.md`

---

## Table of Contents

1. [Memory Hierarchy Overview](#1-memory-hierarchy-overview)
2. [ARM32 Memory Model (ARMv7 + LPAE)](#2-arm32-memory-model-armv7--lpae)
3. [ARM64 Memory Model (ARMv8-A)](#3-arm64-memory-model-armv8-a)
4. [MMU State at `kmem_cache_init` Time](#4-mmu-state-at-kmem_cache_init-time)
5. [struct slab / struct page / struct folio — The MMU Bridge](#5-struct-slab--struct-page--struct-folio--the-mmu-bridge)
6. [Page Allocation for Slab Pages](#6-page-allocation-for-slab-pages)
7. [Cache Alignment: ARM32 vs ARM64](#7-cache-alignment-arm32-vs-arm64)
8. [DMA Zones and the Slab Allocator](#8-dma-zones-and-the-slab-allocator)
9. [NUMA Topology on ARM Platforms](#9-numa-topology-on-arm-platforms)
10. [Memory Zones and Slab Mapping](#10-memory-zones-and-slab-mapping)
11. [TLB and Cache Coherency for Slab](#11-tlb-and-cache-coherency-for-slab)
12. [Memory Tagging (MTE) and KASAN Integration](#12-memory-tagging-mte-and-kasan-integration)
13. [Page Table Walk for a kmalloc Pointer (ARM64)](#13-page-table-walk-for-a-kmalloc-pointer-arm64)
14. [ARM32 vs ARM64 Comparison Table](#14-arm32-vs-arm64-comparison-table)
15. [Full System Design Diagram](#15-full-system-design-diagram)

---

## 1. Memory Hierarchy Overview

### 1.1 The Physical-to-Virtual Stack

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  PHYSICAL MEMORY (DRAM)                                                      │
│  [Managed by: memblock (early) → buddy allocator (runtime)]                  │
├─────────────────────────────────────────────────────────────────────────────┤
│  MEMORY ZONES  (DMA / DMA32 / NORMAL / MOVABLE / HIGHMEM[ARM32 only])       │
│  [Managed by: struct zone; per-node zone lists; watermarks]                  │
├─────────────────────────────────────────────────────────────────────────────┤
│  PAGE ALLOCATOR (BUDDY)                                                       │
│  [alloc_pages(), __alloc_pages_node(); orders 0..MAX_PAGE_ORDER]             │
│  [struct page / struct folio metadata in mem_map[] or vmemmap]               │
├─────────────────────────────────────────────────────────────────────────────┤
│  SLAB ALLOCATOR (SLUB)  ← kmem_cache_init() creates this layer             │
│  [kmem_cache_create(), kmalloc(), kmem_cache_alloc()]                        │
│  [Carves buddy pages into fixed-size objects]                                │
├─────────────────────────────────────────────────────────────────────────────┤
│  KERNEL SUBSYSTEMS / DRIVERS                                                  │
│  [task_struct, sk_buff, inode, dma_buf, gpu_channel, ...]                   │
└─────────────────────────────────────────────────────────────────────────────┘
                              ▲
                              │  ARM MMU translates all kernel VAs → PAs
                              │  via page tables in swapper_pg_dir (TTBR1_EL1)
```

### 1.2 Kernel Virtual Address Space (ARM64, 48-bit)

```
0xFFFF_FFFF_FFFF_FFFF ─┐
                        │  KASAN shadow (if enabled)
0xFFFF_FEFE_0000_0000  ─┤
                        │  vmalloc / ioremap / vmap range
0xFFFF_C000_0000_0000  ─┤
                        │  vmemmap (struct page array — all physical pages)
0xFFFF_BFFF_C000_0000  ─┤
                        │  PCI I/O range
0xFFFF_0000_0000_0000  ─┤
                        │  *** LINEAR MAP (direct map of all physical RAM) ***
                        │  kernel text, data, slab objects, buddy metadata
                        │  PA 0x0 → VA __PAGE_OFFSET (typically 0xFFFF_8000_0000_0000)
0x0000_FFFF_FFFF_FFFF  ─┤
                        │  User space (TTBR0)
0x0000_0000_0000_0000  ─┘
```

All slab objects live in the **linear map**. A `kmalloc(N, GFP_KERNEL)` pointer is always in the linear map range. The MMU maps these with simple 1:1 (PA → linear-map VA) page table entries, all with `MT_NORMAL` cache attributes.

### 1.3 Kernel Virtual Address Space (ARM32, 32-bit)

```
0xFFFF_FFFF ─┐
              │  Fixmap (early ioremap, early console)
0xFFF0_0000  ─┤
              │  Vectors page (exception vectors)
0xFFFF_0000  ─┤
              │  vmalloc / ioremap  (VMALLOC_START to VMALLOC_END, typically 128MB)
0xF000_0000  ─┤
              │  *** LINEAR MAP / LOWMEM ***
              │  kernel text, data, slab objects
              │  PA 0x0 → VA PAGE_OFFSET (0xC000_0000 for 3G/1G split)
0xC000_0000  ─┤
              │  User space (0 to TASK_SIZE)
0x0000_0000  ─┘
```

On ARM32 with HIGHMEM: physical memory above ~760MB cannot be directly mapped. Slab allocations that use `GFP_KERNEL` only come from LOWMEM (below the 760MB boundary). HIGHMEM objects require `kmap()`.

---

## 2. ARM32 Memory Model (ARMv7 + LPAE)

### 2.1 Page Table Architecture

```
Without LPAE (Classic MMU — 32-bit PA):
┌──────────────────────────────────────────────────────────┐
│  2-level page tables: PGD + PTE                          │
│  PGD: 4096 entries × 4 bytes = 16KB (one per process)   │
│  Sections: PGD entry maps 1MB at once (no PTE needed)   │
│  PTE: maps 4KB pages (256 entries per page table)        │
│  Physical address: 32 bits (4GB max)                     │
│  swapper_pg_dir: kernel PGD, 16KB at kernel_pg_dir       │
└──────────────────────────────────────────────────────────┘

With LPAE (Large Physical Address Extension — 40-bit PA):
┌──────────────────────────────────────────────────────────┐
│  3-level page tables: PGD + PMD + PTE                    │
│  Descriptor format: 64-bit (8 bytes each)                │
│  PGD: 4 entries (maps 1GB each), 32 bytes total          │
│  PMD: 512 entries × 8 bytes = 4KB                        │
│  PTE: 512 entries × 8 bytes = 4KB                        │
│  Physical address: 40 bits (1TB max)                     │
│  Page size: always 4KB (no large page support in LPAE PTE)│
└──────────────────────────────────────────────────────────┘
```

**Slab implication**: Page table level count affects how many TLB misses a slab page allocation causes. On LPAE systems, a new slab page (first access) requires walking 3 levels vs 2 levels on classic MMU. With ARM32 hardware page table walkers (in hardware on Cortex-A8+), this is typically handled in hardware.

### 2.2 VIPT Cache and Slab Alignment

ARM Cortex-A processors (ARM32) commonly use a **Virtually Indexed, Physically Tagged (VIPT)** L1 cache. This creates a cache aliasing problem:

```
VIPT aliasing scenario:
  Two VAs (VA1, VA2) that map the same PA can have different cache indices
  if (VA1 & cache_size_mask) ≠ (VA2 & cache_size_mask)

  L1 cache size = 32KB, 4-way, 8-byte lines → index bits = VA[11:5]
  If bit 12 differs between two VAs mapping same PA → two cache lines
  for the same physical data → coherency issue

SLUB mitigation via ARCH_SLAB_MINALIGN:
  arch/arm/include/asm/cache.h:
  #if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5)
  #define ARCH_SLAB_MINALIGN  8
  #endif

  This ensures all slab objects are 8-byte aligned. Combined with
  SLAB_HWCACHE_ALIGN (which aligns to full cache line = 32-64B),
  aliasing is avoided because all objects have the same offset within
  any naturally aligned 4KB page.
```

### 2.3 ARM32 Cache Architecture

```c
// arch/arm/include/asm/cache.h
#define L1_CACHE_SHIFT    CONFIG_ARM_L1_CACHE_SHIFT  // Usually 6 = 64B or 5 = 32B
#define L1_CACHE_BYTES    (1 << L1_CACHE_SHIFT)      // 64B (Cortex-A9, A15) or 32B (Cortex-A8)
#define ARCH_DMA_MINALIGN L1_CACHE_BYTES             // DMA buffers must be cache-line aligned

// L1_CACHE_SHIFT values by core:
// Cortex-A5:  5 → 32B cache lines
// Cortex-A8:  6 → 64B cache lines
// Cortex-A9:  6 → 64B cache lines
// Cortex-A15: 6 → 64B cache lines
// Cortex-A7:  6 → 64B cache lines
```

**Impact on SLUB**: `SLAB_HWCACHE_ALIGN` aligns objects to `cache_line_size()`, which reads the hardware CTR register at runtime. For ARM32, this is typically 32B or 64B. Objects smaller than the cache line are aligned to half the cache line size (e.g., 8B objects aligned to 32B on a 64B-line CPU).

### 2.4 ARM32 Memory Attributes for Slab Pages

ARM32 uses a simplified memory type system (controlled by TEX + CB bits in PTE):

```
Normal, Write-Back, Allocate (WB-WA): TEX=001, CB=11
  Used for: all slab pages, kernel data
  Properties: CPU cache fully involved; hardware prefetch; best performance

Device memory: TEX=000, CB=01
  Used for: MMIO, I/O registers
  Properties: no caching; strictly ordered; never used for slab

Strongly Ordered: TEX=000, CB=00
  Used for: shared memory, special devices
  Properties: all accesses serialized; never used for slab
```

Slab pages are always `Normal WB-WA`, set up by `paging_init()` before `kmem_cache_init()` is called. The slab allocator never changes page attributes.

---

## 3. ARM64 Memory Model (ARMv8-A)

### 3.1 Page Table Architecture

```
Standard ARM64 kernel config (4KB pages, 48-bit VA):
┌─────────────────────────────────────────────────────────────────┐
│  4-level page tables: PGD → PUD → PMD → PTE                    │
│  TTBR1_EL1: points to kernel PGD (swapper_pg_dir)              │
│  TTBR0_EL1: points to user PGD (per-process)                   │
│                                                                  │
│  VA[47:39] → PGD index  (9 bits → 512 entries)                 │
│  VA[38:30] → PUD index  (9 bits → 512 entries)                 │
│  VA[29:21] → PMD index  (9 bits → 512 entries)                 │
│  VA[20:12] → PTE index  (9 bits → 512 entries)                 │
│  VA[11:0]  → Page offset (12 bits → 4096B)                     │
│                                                                  │
│  Each descriptor: 64 bits; PA range: up to 48 bits (256TB)     │
│  Block entries: 1GB (PUD), 2MB (PMD) — can skip a level        │
└─────────────────────────────────────────────────────────────────┘

With 64KB pages (CONFIG_ARM64_64K_PAGES):
  3-level tables: PGD → PMD → PTE
  VA[47:42] → PGD (6 bits), VA[41:29] → PMD (13 bits), VA[28:16] → PTE (13 bits)
  VA[15:0] → 64KB page offset
  Useful for large-page DMA, GPU SMMU mappings

With LPA2 (52-bit VA, e.g., ARM64 SVE servers):
  Still 4 levels but PGD indexes 4 more bits
  Enables 4PB+ address spaces
```

### 3.2 ARM64 Cache Architecture

```c
// arch/arm64/include/asm/cache.h
#define L1_CACHE_SHIFT    6                           // Always 64-byte lines on ARMv8
#define L1_CACHE_BYTES    (1 << L1_CACHE_SHIFT)       // 64 bytes
#define ARCH_DMA_MINALIGN 128                         // DMA coherency granule = 2 cache lines
                                                       // Prevents DMA "false sharing" with
                                                       // adjacent CPU-cached data
#define ARCH_KMALLOC_MINALIGN 8                        // Minimum slab object alignment

// MTE (Memory Tagging Extension) path:
#ifdef CONFIG_KASAN_HW_TAGS
static inline unsigned int arch_slab_minalign(void) {
    return kasan_hw_tags_enabled() ? MTE_GRANULE_SIZE   // 16 bytes
                                   : __alignof__(unsigned long long);
}
#define arch_slab_minalign() arch_slab_minalign()
#endif
```

**ARM64 uses PIPT (Physically Indexed, Physically Tagged) L1 caches**: No aliasing problem. The 64-byte cache line is universal across all ARM64 Cortex-A and Neoverse cores. The Neoverse V1/V2 (used in AWS Graviton3/4) uses 64-byte lines with 512KB L2 and up to 64MB L3.

### 3.3 ARM64 Memory Attributes (MAIR_EL1)

```
MAIR_EL1 register defines up to 8 memory attribute types (indices 0-7):

Index 0: Device-nGnRnE (Strongly Ordered)  — MMIO, PCIe config
Index 1: Device-nGnRE (Device)             — DMA coherent device memory
Index 2: Normal, Non-Cacheable             — Uncached (rare, for special DMA)
Index 3: Normal, WB-WA                     — All kernel data, slab pages
Index 4: Normal, WT                        — Write-through (rarely used)

PTE bits AttrIndx[2:0] select which MAIR entry applies.

For slab pages (set by create_mapping in arch/arm64/mm/mmu.c):
  AttrIndx = 3 (Normal WB-WA)
  SH[1:0]  = 11 (Inner Shareable — coherent across all CPUs in the cluster)
  AF       = 1  (Access Flag set — hardware sets this on first access)
  AP[2:1]  = 00 (EL1 read/write, EL0 no access for kernel memory)
```

### 3.4 ARM64 Linear Map Setup (Slab Pre-requisite)

The linear map is set up in `paging_init()` (called from `setup_arch()`), long before `kmem_cache_init()`. For every page frame of physical RAM:

```
create_mapping(phys_addr, __phys_to_virt(phys_addr), PAGE_SIZE, prot_kernel)
  → fills swapper_pg_dir PGD/PUD/PMD/PTE entries
  → PTE: PA | PTE_VALID | PTE_AF | PTE_SHARED | PTE_ATTRINDX(MT_NORMAL) | PTE_WRITE
```

So by the time `kmem_cache_init` calls `new_slab()` → `__alloc_pages_node()` → buddy returns page P:
- `page_to_virt(P)` gives a valid VA in the linear map
- That VA already has a valid PTE in swapper_pg_dir
- No TLB fault will occur when accessing slab memory

---

## 4. MMU State at `kmem_cache_init` Time

### 4.1 What Is Already Set Up

```
By the time kmem_cache_init() is entered at mm/mm_init.c:2722:

ARM32:
  ✓ SCTLR.M=1  — MMU enabled
  ✓ TTBR0 = swapper_pg_dir (identity map + kernel map)
  ✓ DOMAIN control registers set (kernel domain AP=01)
  ✓ L1 I-cache and D-cache enabled (SCTLR.C=1, SCTLR.I=1)
  ✓ Linear map covers all lowmem physical RAM
  ✓ memblock handed all pages to buddy (memblock_free_all done)

ARM64:
  ✓ SCTLR_EL1.M=1  — MMU enabled
  ✓ TTBR1_EL1 = swapper_pg_dir (kernel PGD)
  ✓ TTBR0_EL1 = empty_zero_page (no user mappings yet)
  ✓ MAIR_EL1 set with 8 memory types
  ✓ TCR_EL1 configured for 48-bit VA, 4KB granule
  ✓ Linear map covers all physical RAM
  ✓ vmemmap area mapped (struct page array accessible)
  ✓ All caches (L1 D-cache, L2) enabled and coherent
```

### 4.2 What `kmem_cache_init` Does NOT Touch

- **Does not** modify any page tables (no `create_mapping()` calls)
- **Does not** modify `MAIR_EL1`, `TCR_EL1`, or any system registers
- **Does not** flush any TLBs or caches
- **Does not** modify memory zones or buddy allocator state

It purely creates **software metadata** (slab cache descriptors) and uses the **already-mapped** linear map via `page_to_virt()`.

### 4.3 The `slab_address()` Implementation

```c
// How SLUB gets the virtual address of a slab page:
static inline void *slab_address(const struct slab *slab) {
    return folio_address(slab_folio(slab));
    // = page_address(compound_head(slab_page(slab)))
    // = __va(page_to_pfn(page) << PAGE_SHIFT)
    // = (void *)(PAGE_OFFSET + (page_to_pfn(page) << PAGE_SHIFT))
    //                           ^^^^^^^^^^ linear map offset
}
```

ARM64 linear map: `__va(pa) = pa - PHYS_OFFSET + PAGE_OFFSET`
ARM32 linear map: `__va(pa) = pa - PHYS_OFFSET + PAGE_OFFSET` (where PAGE_OFFSET = 0xC0000000)

---

## 5. `struct slab` / `struct page` / `struct folio` — The MMU Bridge

### 5.1 The `mem_map[]` and `vmemmap` Arrays

```
ARM32 (FLATMEM or DISCONTIGMEM):
  mem_map[] = global array of struct page, one entry per PFN
  Address of struct page for PFN N = mem_map + N
  Stored in lowmem; allocated by memblock during paging_init

ARM64 (SPARSEMEM + VMEMMAP):
  vmemmap = virtual array at VMEMMAP_START (arch/arm64/include/asm/memory.h)
  Address of struct page for PFN N = vmemmap + N
  The vmemmap pages themselves are backed by physical pages,
  mapped in the vmemmap region (separate from the linear map)

  VMEMMAP_START = -(UL(1) << (VA_BITS - 1))  on 48-bit systems
               ≈ 0xFFFF_BFFF_C000_0000  for 48-bit VA

  Benefit: sparse physical memory supported without wasting mem_map holes;
           huge pages can back vmemmap for large systems (vmemmap_populate_hugepages)
```

### 5.2 The `struct slab` Overlay

The key design decision: `struct slab` shares memory with `struct page` using a union-like overlay:

```
Physical layout (same memory address):

Offset 0:   struct page::flags         = struct slab::flags
Offset 8:   struct page::compound_head = struct slab::slab_cache  ← bit 0 must be 0!
Offset ...: struct page::lru           ≈ struct slab::slab_list

This works because:
  - Slab pages are always compound page HEADS (bit 0 of compound_head = 0)
  - Tail pages have bit 0 of compound_head = 1 (to distinguish them)
  - So slab->slab_cache (even-aligned pointer) naturally has bit 0 = 0

Compile-time verification:
  SLAB_MATCH(flags, flags);              // offsets must match
  SLAB_MATCH(compound_head, slab_cache); // offsets must match
  static_assert(sizeof(struct slab) <= sizeof(struct page));
```

### 5.3 Why This Design?

Before `struct slab` was introduced, SLUB used `struct page` directly to track slab metadata. This created:
- **Coupling** between MM and slab internals
- **ABI fragility** when `struct page` changed
- **Type confusion** — hard to distinguish page uses

The overlay approach gives SLUB its own type-safe interface while keeping zero overhead (no extra memory per slab page).

### 5.4 Conversion Chain

```c
// From a virtual address (e.g., from kmalloc):
void *ptr;
struct folio *folio = virt_to_folio(ptr);        // linear map → folio
struct slab  *slab  = folio_slab(folio);          // folio → slab (type check)
struct kmem_cache *s = slab->slab_cache;           // back-pointer to cache
unsigned int index = obj_to_index(s, slab, ptr);  // which slot (uses reciprocal_size)

// From a struct page:
struct page *page = virt_to_page(ptr);
struct slab *slab = page_slab(page);              // = (struct slab *)page

// To get the data address of a slab:
void *data = slab_address(slab);                  // = folio_address(slab_folio(slab))
```

---

## 6. Page Allocation for Slab Pages

### 6.1 The Call Chain

```
new_slab(kmem_cache_node, GFP_NOWAIT, node)    [mm/slub.c]
└── allocate_slab(s, flags, node)
    └── alloc_slab_page(s, alloc_gfp, node, oo) [mm/slub.c]
        └── alloc_pages_node(node, alloc_gfp, oo_order(oo))
            └── __alloc_pages_node(node, gfp, order)
                └── __alloc_pages(gfp, order, preferred_nid, nodemask)
                    └── get_page_from_freelist(gfp, order, alloc_flags, &ac)
                        └── rmqueue(zone, order, gfp_flags, alloc_flags, migratetype)
                            → returns struct page *
```

### 6.2 GFP Flags Used

| Scenario | GFP Flags | Notes |
|---|---|---|
| Bootstrap (`early_kmem_cache_node_alloc`) | `GFP_NOWAIT` | Cannot sleep; no reclaim; returns NULL if unavailable |
| Normal slab page alloc | `s->allocflags \| GFP_KERNEL` | Can sleep; reclaim allowed |
| DMA slab cache | `s->allocflags` includes `GFP_DMA` | Allocate from DMA zone |
| RECLAIM cache | `s->allocflags` includes `__GFP_RECLAIMABLE` | Pages counted as reclaimable |
| Compound page (always) | `__GFP_COMP` | Multiple pages treated as one unit; required for order>0 |

### 6.3 Compound Pages and ARM64 Folios

```
For an order=1 slab (2 pages = 8KB), the buddy allocator returns:
  page[0]: compound head — flags has PG_head set
  page[1]: compound tail — compound_head points back to page[0] with bit0=1

After SLUB marks page[0] as a slab:
  page[0].flags has PG_slab set (checked by PageSlab())
  page[0] is cast to struct slab * for all slab operations
  page[1] is a tail; folio_page(folio, 1) reaches it but SLUB only uses page[0]

ARM64 folio representation:
  struct folio = compound head page
  folio_size(folio) = 2^order * PAGE_SIZE = 8KB for order=1
  folio_pfn(folio) = page_to_pfn(&folio->page)
  folio_address(folio) = __va(folio_pfn(folio) << PAGE_SHIFT) = linear map VA
```

---

## 7. Cache Alignment: ARM32 vs ARM64

### 7.1 The `SLAB_HWCACHE_ALIGN` Effect

When `SLAB_HWCACHE_ALIGN` is set (always set for bootstrap caches, most kernel caches):

```c
// mm/slab_common.c — in calculate_alignment():
unsigned int align = ARCH_KMALLOC_MINALIGN;  // baseline

if (flags & SLAB_HWCACHE_ALIGN) {
    unsigned int ralign = cache_line_size();  // reads hardware CTR register
    // Objects larger than cache line: align to cache line
    // Objects smaller: align to half, quarter, etc. down to ARCH_SLAB_MINALIGN
    while (size <= ralign / 2)
        ralign /= 2;
    align = max(align, ralign);
}
return ALIGN(align, sizeof(void *));
```

### 7.2 ARM32 Alignment Specifics

```
Cortex-A9 (typical embedded ARM32 SoC):
  L1 D-cache: 32KB, 4-way, 32-byte lines
  cache_line_size() = 32

  kmem_cache_node (sizeof = ~80 bytes):
    ralign = 32; size(80) > 32/2(16) → align = 32
    Result: kmem_cache_node objects aligned to 32-byte boundaries

  kmem_cache (sizeof ~ 256 bytes):
    ralign = 32; size(256) > 16 → align = 32
    Result: kmem_cache objects aligned to 32 bytes

  kmalloc-8:
    ralign = 32; 8 <= 32/2=16, so ralign/=2=16; 8 <= 8, so ralign/=2=8
    align = max(ARCH_KMALLOC_MINALIGN, 8) = 8
    Result: 8-byte objects, 8-byte aligned (ARCH_SLAB_MINALIGN = 8 for EABI)

VIPT concern (ARM32 only):
  Objects naturally 32-byte aligned within a 4KB page are safe from VIPT aliasing
  because bits [11:5] (index bits for 32-byte lines, 32KB 4-way cache) are
  determined solely by the page offset, and page offset is identical for any
  two VAs mapping the same physical page that are in the linear map
  (there's only ONE linear map VA for each PA — no aliasing possible in linear map).
  
  VIPT aliasing risk exists only in user space or via kmap/vmap with different VAs.
  Slab is immune because it exclusively uses the linear map.
```

### 7.3 ARM64 Alignment Specifics

```
Cortex-A72 / Neoverse N1 (common ARM64 server/SoC):
  L1 D-cache: 32KB or 64KB, 4-way, 64-byte lines
  cache_line_size() = 64

  kmem_cache_node (sizeof ~ 104 bytes on 64-bit):
    ralign = 64; size(104) > 32 → align = 64
    Objects aligned to 64-byte boundaries (fits 2 per 128B cache pair)

  kmalloc-64:
    ralign = 64; size(64) > 32 → align = 64
    Each object occupies exactly 1 cache line — perfect packing, zero false sharing

  kmalloc-8 on DMA system (ARCH_DMA_MINALIGN=128):
    minalign = max(ARCH_KMALLOC_MINALIGN=8, dma_get_cache_alignment()=128) = 128
    aligned_size = ALIGN(8, 128) = 128
    → kmalloc-8 cache stores 128-byte objects! (120 bytes wasted per object)
    This is why ARCH_DMA_MINALIGN=128 (two cache lines) matters:
    DMA writes must not share a cache line with CPU data
    (prevents DMA coherency bug where CPU write invalidates DMA data)

ARM64 PIPT — NO aliasing concern:
  Physical tag = eliminates VIPT aliasing entirely
  Any number of VAs can map the same PA; cache is indexed by PA
  Still need HWCACHE_ALIGN for performance (false sharing prevention)
```

### 7.4 False Sharing Example

```
Without SLAB_HWCACHE_ALIGN (24-byte objects on 64-byte line ARM64):
  ┌────────────────────────────────────────────────────────────┐
  │ Cache line 0 (64 bytes)                                    │
  │ [obj0: 24B][obj1: 24B][obj2_part: 16B]                    │
  │                                                            │
  │ CPU-A modifies obj0 → invalidates entire 64B cache line   │
  │ CPU-B accessing obj1 → cache miss! Must re-fetch          │
  └────────────────────────────────────────────────────────────┘

With SLAB_HWCACHE_ALIGN (24→64 byte padding, aligned to 64):
  ┌────────────────────────────────────────────────────────────┐
  │ Cache line 0: [obj0: 24B + 40B padding]                   │
  ├────────────────────────────────────────────────────────────┤
  │ Cache line 1: [obj1: 24B + 40B padding]                   │
  └────────────────────────────────────────────────────────────┘
  CPU-A modifies obj0 → only cache line 0 invalidated
  CPU-B accesses obj1 → cache line 1 unaffected ✓
```

---

## 8. DMA Zones and the Slab Allocator

### 8.1 ARM32 DMA Zone

```
ARM32 ISA DMA limitation (legacy from PC/AT ISA bus):
  Zone DMA: PA 0x00000000 → 0x00FFFFFF (0 → 16MB)
  Zone DMA32: Not applicable on 32-bit ARM (all memory fits in 32 bits)
  Zone NORMAL: PA 16MB → end of lowmem
  Zone HIGHMEM: PA above lowmem boundary (e.g., > 760MB)

slab_cache_dma flag effect on ARM32:
  s->allocflags |= GFP_DMA  → alloc from zone DMA only
  Zone DMA on ARM32 is first 16MB — same DMA as legacy PC ISA
  Modern ARM32 SoCs often have IOMMU making this moot,
  but kernel maintains legacy for older platforms

kmalloc DMA caches on ARM32:
  kmalloc-dma-8, kmalloc-dma-16, ..., kmalloc-dma-4k
  Created by new_kmalloc_cache(idx, KMALLOC_DMA)
  All from zone DMA (first 16MB of PA)
```

### 8.2 ARM64 DMA Zone

```
ARM64 zone layout (no legacy ISA):
  Zone DMA: PA 0x00 → 0x3FFF_FFFF (0 → 1GB) — only if CONFIG_ZONE_DMA
  Zone DMA32: PA 0x00 → 0xFFFF_FFFF (0 → 4GB) — if CONFIG_ZONE_DMA32
  Zone NORMAL: all remaining physical memory
  No HIGHMEM zone (64-bit VA can map all physical memory)

ARCH_DMA_MINALIGN = 128 (2 × cache line):
  DMA transfers must be cache-line aligned to prevent coherency issues:
  
  Problem without alignment:
    CPU writes to bytes [0..7] of a cache line
    DMA writes to bytes [8..63] of SAME cache line
    CPU has dirty cache line → CPU's write wins, DMA write lost!
  
  Solution: DMA buffer occupies complete cache lines
    kmalloc for DMA: ALIGN(requested_size, ARCH_DMA_MINALIGN=128)
    Ensures DMA and CPU data never share a cache line

  With hardware I/O coherency (most ARM64 platforms with SMMU/GIC):
    ARCH_DMA_MINALIGN may be reduced to 1 (no forced alignment needed)
    because the coherency interconnect handles CPU↔DMA coherency
```

### 8.3 DMA Coherency Impact on kmalloc Sizes

```
System: ARM64 Cortex-A72, no I/O coherency, ARCH_DMA_MINALIGN=128

new_kmalloc_cache(3, KMALLOC_NORMAL):
  kmalloc_info[3].size = 8
  minalign = __kmalloc_minalign() = max(ARCH_KMALLOC_MINALIGN=8, dma_align=1) = 8
  aligned_size = ALIGN(8, 8) = 8 → creates kmalloc-8 at 8-byte object size

new_kmalloc_cache(3, KMALLOC_DMA):
  kmalloc_info[3].size = 8
  minalign = max(ARCH_KMALLOC_MINALIGN=8, dma_get_cache_alignment()=128) = 128
  aligned_size = ALIGN(8, 128) = 128
  aligned_idx = __kmalloc_index(128) = 7 (maps to kmalloc-128 slot!)
  → kmalloc_caches[KMALLOC_DMA][3] = kmalloc_caches[KMALLOC_DMA][7]
  → DMA 8-byte allocations get 128-byte aligned objects from kmalloc-dma-128
```

---

## 9. NUMA Topology on ARM Platforms

### 9.1 ARM32: Mostly UMA

```
Typical ARM32 embedded SoC (Cortex-A9 quad-core):
  nr_node_ids = 1
  slab_nodes = {node 0}
  
  kmem_cache->node[] has only one entry: node[0]
  All slab allocations go to/from NUMA node 0
  No cross-node overhead; for_each_kmem_cache_node loops once

kmem_cache size on ARM32 UMA:
  offsetof(struct kmem_cache, node) + 1 * sizeof(ptr)
  = offsetof(...) + 4  (32-bit pointer)
```

### 9.2 ARM64: Real NUMA on Server Platforms

```
Platform examples with ARM64 NUMA:
  ┌─────────────────────────────────────────────────────────┐
  │ Cavium ThunderX2 (32-core NUMA):                        │
  │   2 sockets, each = 1 NUMA node                        │
  │   nr_node_ids = 2                                       │
  │   Cross-node latency: ~100ns vs 10ns local              │
  │                                                         │
  │ Ampere Altra (80-core Neoverse N1):                     │
  │   Typically UMA per socket; 2-socket = 2 nodes          │
  │   Slab partitioned: each node's kmem_cache_node         │
  │   allocated on its local node                           │
  │                                                         │
  │ AWS Graviton3 (64 cores Neoverse V1):                   │
  │   Single node per instance (UMA in practice)            │
  │   Unless using bare metal with 2 sockets                │
  └─────────────────────────────────────────────────────────┘

SLUB NUMA behavior:
  init_kmem_cache_nodes() → kmem_cache_alloc_node(kmem_cache_node, GFP_KERNEL, node)
  → allocates struct kmem_cache_node from node-local memory
  → ensures kmem_cache_node metadata accessed by node-N CPUs is in node-N DRAM

  s->node[0] → struct kmem_cache_node on NUMA node 0 (8 bytes in node-0 DRAM)
  s->node[1] → struct kmem_cache_node on NUMA node 1 (8 bytes in node-1 DRAM)

remote_node_defrag_ratio (struct kmem_cache field):
  Controls when node-remote partial slabs are used to avoid fragmentation:
  0   = never use remote node slabs (maximize locality, accept fragmentation)
  100 = always use remote node slabs (minimize fragmentation, accept latency)
  Default: 1000/nr_node_ids (scales with NUMA size)
```

### 9.3 NUMA-Aware Allocation

```c
// Driver code doing NUMA-aware allocation:
struct gpu_state *s = kmem_cache_alloc_node(gpu_state_cache,
                                              GFP_KERNEL,
                                              dev_to_node(gpu_device));

// Internally, SLUB does:
// 1. Identify target node N from the 'node' argument
// 2. Get this CPU's per-cpu sheaf → if has objects, return immediately
// 3. If no local objects: check s->node[N]->barn for full sheaves
// 4. If barn empty: allocate new slab page from __alloc_pages_node(N, ...)
//    → buddy allocator on node N → physically local DRAM
```

---

## 10. Memory Zones and Slab Mapping

### 10.1 Zone → Physical Range → Slab Cache Mapping

| Zone | ARM32 PA Range | ARM64 PA Range | GFP Flag | kmalloc Cache Type |
|---|---|---|---|---|
| `ZONE_DMA` | 0 → 16MB | 0 → 1GB (if CONFIG_ZONE_DMA) | `GFP_DMA` | `KMALLOC_DMA` |
| `ZONE_DMA32` | N/A | 0 → 4GB (if CONFIG_ZONE_DMA32) | `GFP_DMA32` | `KMALLOC_DMA` |
| `ZONE_NORMAL` | 16MB → lowmem | 1GB (or 4GB) → end | `GFP_KERNEL` | `KMALLOC_NORMAL`, `KMALLOC_RECLAIM`, `KMALLOC_CGROUP` |
| `ZONE_HIGHMEM` | Above lowmem (~760MB) | Not present | `__GFP_HIGHMEM` | Not used by slab (slab needs linear map) |
| `ZONE_MOVABLE` | Configurable | Configurable | `__GFP_MOVABLE` | Not directly (CMA uses this) |

### 10.2 Why SLUB Doesn't Use HIGHMEM

SLUB requires linearly mapped memory for `slab_address()` to work:

```c
// slab_address(slab) = folio_address(slab_folio(slab)) = page_to_virt(page)
// page_to_virt() only works for linear-map pages!
// HIGHMEM pages don't have a permanent linear map VA (only kmap'd temporarily)

// ARM32 HIGHMEM: pages above ~760MB have no fixed VA
//   → cannot use page_to_virt() → cannot compute slab object addresses
//   → slab allocator can ONLY use LOWMEM (ZONE_NORMAL)
```

This is why `GFP_KERNEL` slab allocations on ARM32 with lots of RAM still come from the first 768MB — slab cannot use HIGHMEM.

### 10.3 Memory Accounting

```c
// NR_SLAB_RECLAIMABLE: pages in KMALLOC_RECLAIM caches
//   → shown in /proc/meminfo as "Slab: X kB" / "SReclaimable: X kB"
//   → kswapd uses this to decide on memory pressure

// NR_SLAB_UNRECLAIMABLE: pages in KMALLOC_NORMAL/CGROUP caches
//   → "SUnreclaim: X kB" in /proc/meminfo
//   → Cannot be reclaimed under memory pressure (kernel needs them)

// SLUB_DEBUG enabled:
//   /sys/kernel/slab/<name>/alloc_calls
//   /sys/kernel/slab/<name>/free_calls
//   /sys/kernel/slab/<name>/objects_partial
```

---

## 11. TLB and Cache Coherency for Slab

### 11.1 ARM64 Hardware Cache Coherency

On ARMv8-A, the inner shareable domain (all cores in a cluster, or all cores system-wide with DSU) maintains **hardware coherency**:

```
CPU-A writes to slab object at VA:0xFFFF8000_00100040
  → hits L1 D-cache (tagged by PA, PIPT)
  → snoops L1 caches of all CPUs in inner shareable domain
  → CPU-B's stale cache line for same PA is automatically invalidated

No explicit cache flush needed between CPUs for slab operations!
This is why SLUB's cross-CPU object transfer (sheaf exchange via barn) works
without cache management instructions.
```

### 11.2 ARM32 Cache Coherency

ARM32 (Cortex-A9 with SCU):
```
SCU (Snoop Control Unit) maintains coherency for cores in the cluster.
  - L1 D-cache: VIPT, coherent via SCU
  - L2 (external, e.g., PL310): physically tagged, coherent
  - Cross-cluster: depends on platform fabric

For slab: no explicit flushes needed because:
  1. All slab VAs are in the linear map (single VA per PA → no VIPT aliasing)
  2. SCU ensures cross-core coherency within the cluster
  3. L2 cache controller handles cross-cluster if applicable
```

### 11.3 Memory Barriers in SLUB Spinlocks

ARM64 `spin_lock` / `spin_unlock` include DMB (Data Memory Barrier):

```asm
// spin_lock equivalent (ARM64 simplified):
spin_lock:
    SEVL                    // Optional: signal event to self
1:  LDAXR x0, [lock]       // Load-Acquire Exclusive
    CBNZ x0, 2f            // If non-zero (locked), spin
    STXR w1, #1, [lock]    // Store Exclusive
    CBNZ w1, 1b            // If store failed, retry
    // LDAXR includes a load-acquire barrier (DMB ISHLD equivalent)
    RET

spin_unlock:
    STLR xzr, [lock]       // Store-Release (includes DMB ISHST equivalent)
    RET
```

The Load-Acquire in `LDAXR` ensures that all memory reads/writes between lock acquisition and subsequent operations are ordered. The Store-Release in `STLR` ensures all preceding operations are visible before the lock is released.

For the SLUB per-cpu trylock (`local_trylock_t`), ARM64 uses:
```c
// local_trylock is lighter: only protects against preemption on same CPU
// No DMB needed — single CPU is inherently ordered (program order)
local_trylock_init(&pcs->lock);
local_trylock(&pcs->lock);   // disables preemption + trylock
local_tryunlock(&pcs->lock); // re-enables preemption
```

### 11.4 Cross-CPU Free (kfree from different CPU)

```
CPU-A allocates object O from slab S (via kmem_cache_alloc)
CPU-B frees object O (via kfree(O))

Old SLUB (pre-sheaves):
  CPU-B: must acquire slab->list_lock (spinlock), modify freelist
  → cache line bouncing of slab metadata page between CPU-A and CPU-B L1 caches

New SLUB (sheaves):
  CPU-B: push O to local sheaf (own CPU's per-cpu sheaf, no lock)
  → sheaf eventually flushed to barn (spinlock, but batched)
  → barn spinlock is only acquired once per N objects (batch size)
  → cache line bouncing reduced by factor of N (sheaf capacity)
```

---

## 12. Memory Tagging (MTE) and KASAN Integration

### 12.1 ARM64 MTE Overview

```
Memory Tagging Extension (ARMv8.5-A):
  4-bit tag stored in physical DRAM alongside each 16-byte granule
  (extra tag memory ~3% overhead, managed by hardware)
  
  Tagged pointer: bits [59:56] of VA = 4-bit tag
  Hardware check: on every memory access, compare pointer tag vs memory tag
  On mismatch: tag fault (SIGSEGV or kernel fault)

  Granule size: 16 bytes (MTE_GRANULE_SIZE = 16)
  → ALL slab objects must be 16-byte aligned for MTE
  → arch_slab_minalign() = MTE_GRANULE_SIZE = 16 (when kasan_hw_tags_enabled())
```

### 12.2 MTE Impact on SLUB

```c
// arch/arm64/include/asm/cache.h
static inline unsigned int arch_slab_minalign(void) {
    return kasan_hw_tags_enabled() ? MTE_GRANULE_SIZE : __alignof__(unsigned long long);
}

// Effect in calculate_alignment() for kmalloc-8 with MTE:
// minalign = max(ARCH_KMALLOC_MINALIGN=8, arch_slab_minalign()=16) = 16
// kmalloc-8 object size = ALIGN(8, 16) = 16 bytes (not 8!)
// → each kmalloc-8 allocation gets a tagged 16-byte slot

// kasan_slab_alloc() when MTE enabled:
// 1. Generate random 4-bit tag
// 2. Set memory tag for object's granules (IRG + STG instructions)
// 3. Return pointer with tag in bits[59:56]
// 4. On use-after-free: memory tag reset to 0 after free → pointer tag mismatch → fault
```

### 12.3 KASAN SW Tags (ARM64, Software Implementation)

```
KASAN Software Tag-Based (CONFIG_KASAN_SW_TAGS):
  Shadow memory: 1 byte per 16B granule (stored in kernel VA space)
  KASAN shadow base: KASAN_SHADOW_OFFSET
  Shadow VA = (object_va >> 4) + KASAN_SHADOW_OFFSET

  Effect on slab:
    kasan_cache_create(s, &size, &s->flags):
      - Adds 1 shadow byte per 16B of object (stored after object in slab?)
      - Actually: shadow is global; no per-object overhead in slab page
      - But: ARCH_SLAB_MINALIGN = (1ULL << KASAN_SHADOW_SCALE_SHIFT) = 16
        → objects 16-byte aligned for shadow granule accuracy

  On allocation:
    kasan_slab_alloc() sets shadow bytes to tag (not 0xFF poison)
    Sets top byte of returned pointer to random tag
  
  On free:
    kasan_slab_free() sets shadow bytes to KASAN_TAG_INVALID (0xFF)
    Any subsequent access → shadow check fails → KASAN report
```

### 12.4 KASAN Generic (ARM32 and ARM64 without MTE)

```
KASAN Generic (CONFIG_KASAN_GENERIC):
  Shadow: 1 byte per 8B of kernel memory (12.5% memory overhead)
  Shadow range: [KASAN_SHADOW_START, KASAN_SHADOW_END]

  ARM32: KASAN shadow mapped at top of address space
  ARM64: KASAN shadow at fixed offset in kernel VA

  For slab:
    kasan_cache_create(): may increase s->size for per-object KASAN metadata
    Objects tracked via shadow memory (no MTE hardware needed)
    Redzone regions marked with specific shadow values
    Free object memory marked with KASAN_FREE_PAGE (0xFF)
```

---

## 13. Page Table Walk for a kmalloc Pointer (ARM64)

### 13.1 Step-by-Step Walk

```
Example: kmalloc(64, GFP_KERNEL) returns ptr = 0xFFFF800012340040

Step 1: Identify address space
  ptr[63] = 1 → kernel address (TTBR1_EL1 space, not TTBR0)
  ptr[48:63] = 0xFFFF → valid canonical kernel VA

Step 2: Load TTBR1_EL1
  MRS x0, TTBR1_EL1 → physical address of swapper_pg_dir (PGD)
  e.g., TTBR1_EL1 = 0x0000_0000_4000_0000 (physical)

Step 3: PGD lookup
  PGD index = ptr[47:39] = bits 39-47 of 0xFFFF800012340040
  = bits of 0x0000800012340040 after masking = index 0
  PGD entry 0 = &swapper_pg_dir[0] = load 8 bytes
  → PGD entry contains: PUD base PA | valid | table descriptor

Step 4: PUD lookup
  PUD index = ptr[38:30] = (0xFFFF800012340040 >> 30) & 0x1FF = 2
  PUD entry 2 = load 8 bytes from PUD base + 2*8
  → PUD entry: PMD base PA | valid | table descriptor
  (Or a 1GB block entry if huge page: skip PMD+PTE)

Step 5: PMD lookup
  PMD index = ptr[29:21] = (0xFFFF800012340040 >> 21) & 0x1FF = 9
  PMD entry 9 = load 8 bytes from PMD base + 9*8
  → PMD entry: PTE base PA | valid | table descriptor
  (Or a 2MB block entry if huge page: skip PTE)

Step 6: PTE lookup
  PTE index = ptr[20:12] = (0xFFFF800012340040 >> 12) & 0x1FF = 52 (0x34)
  PTE entry 52 = load 8 bytes from PTE base + 52*8
  → PTE = 0x0040_0000_1234_0743 (example)
  
  PTE bit breakdown:
    Bits[47:12]: Output PA = 0x1234_0000  (the physical slab page)
    Bit[0]:     Valid = 1
    Bit[1]:     Table/Page = 1 (page descriptor)
    Bits[4:2]:  AttrIndx = 011 (Normal WB-WA, MAIR index 3)
    Bits[9:8]:  SH = 11 (Inner Shareable — coherent with all CPUs)
    Bit[6]:     AP[2] = 0 (EL1 writable)
    Bit[7]:     AP[1] = 0 (EL0 no access — kernel page)
    Bit[10]:    AF = 1 (Access Flag — set on first access)
    Bit[11]:    nG = 0 (global — in ASID-independent TLB)
    Bit[51]:    DBM = 0 (no dirty bit management)
    Bit[53]:    PXN = 0 (privileged execute OK if needed)
    Bit[54]:    UXN = 1 (user execute never — kernel data, not code)

Step 7: Form physical address
  PA = (PTE[47:12] << 12) | ptr[11:0]
     = 0x1234_0000 | 0x040
     = 0x0000_0000_1234_0040

Step 8: L1 cache lookup (PIPT)
  Tag = PA[47:6] = 0x48D010 (cache tag for 64-byte line)
  Index = PA[11:6] = 1 (cache set index)
  Offset = PA[5:0] = 0 (byte within cache line)
  If hit → return data; if miss → fetch from L2/L3/DRAM
```

### 13.2 TLB Role

```
ARM64 TLB (Translation Lookaside Buffer):
  - Caches VA → PA translations
  - Tagged by ASID (Address Space ID) for user entries
  - Kernel entries (nG=0) are ASID-agnostic (global)
  - Slab page translations: ASID-independent, shared across all processes

After create_mapping() in paging_init():
  All linear map PTEs are loaded; TLB entries populated on first access (AF=0→1)
  
  When kmalloc allocates a new slab page:
    - Page is already mapped (linear map covers all RAM)
    - TLB may have stale or no entry for that PFN
    - First access: TLB miss → hardware page table walk → TLB filled → AF bit set
    - Subsequent accesses: TLB hit (no page table walk needed)
    - No explicit TLB flush needed after buddy allocation (page was always mapped)
```

---

## 14. ARM32 vs ARM64 Comparison Table

| Feature | ARM32 (ARMv7 + LPAE) | ARM64 (ARMv8-A) |
|---|---|---|
| **Page table levels** | 2 (no LPAE) or 3 (LPAE) | 4 (4K/16K) or 3 (64K) |
| **Virtual address bits** | 32 bits (4GB VA) | 48 bits (256TB VA), 52 with LPA2 |
| **Physical address bits** | 32 bits (no LPAE) or 40 bits (LPAE) | 48 bits (256TB), 52 with LPA2 |
| **Page size** | 4KB (small), 64KB (large), 1MB (section) | 4KB, 16KB, or 64KB (configured at build time) |
| **Cache type** | VIPT (most Cortex-A) | PIPT (all ARMv8-A) |
| **L1 cache line** | 32B (A5/A8) or 64B (A9/A15/A7) | Always 64B |
| **`ARCH_SLAB_MINALIGN`** | 8 (EABI requirement) | 8 (default), 16 (MTE) |
| **`ARCH_KMALLOC_MINALIGN`** | Not defined (uses L1_CACHE_BYTES) | 8 |
| **`ARCH_DMA_MINALIGN`** | `L1_CACHE_BYTES` (32 or 64B) | 128B (2 cache lines) |
| **`ZONE_DMA`** | First 16MB | First 1GB (if configured) |
| **`ZONE_DMA32`** | Not applicable | First 4GB (if configured) |
| **`ZONE_HIGHMEM`** | Yes (above ~760MB) | No (64-bit VA maps all PA) |
| **vmemmap** | No (uses `mem_map[]` global) | Yes (at `VMEMMAP_START`) |
| **MTE support** | No | Yes (ARMv8.5-A+) |
| **KASAN HW tags** | No | Yes (via MTE) |
| **KASAN SW tags** | Possible but rare | Supported |
| **TLB global entries** | Yes (kernel uses non-ASID entries) | Yes (`nG=0` in PTE) |
| **SMP coherency** | SCU (Snoop Control Unit) | DSU (DynamIQ Shared Unit) / GIC interconnect |
| **NUMA** | Rare (single-node typical) | Common on server platforms |
| **Kernel linear map** | `PAGE_OFFSET` = 0xC0000000 | `PAGE_OFFSET` ≈ 0xFFFF8000_00000000 |
| **Max slab object** | 4MB (`KMALLOC_MAX_SIZE`) | 8MB (depends on `MAX_PAGE_ORDER`) |
| **Compound page order** | Up to `MAX_PAGE_ORDER` (10) | Up to `MAX_PAGE_ORDER` (10+) |
| **Spinlock implementation** | LDREX/STREX + DMB | LDAXR/STXR (load-acquire/store-release) |
| **Memory barriers in spin_lock** | `DMB ISHST` + `DSB ISH` | Load-Acquire in `LDAXR` |

---

## 15. Full System Design Diagram

### 15.1 System Design: Physical Memory → SLUB → Driver Object

```
╔═══════════════════════════════════════════════════════════════════════╗
║              PHYSICAL MEMORY (DRAM)                                   ║
║  ┌──────────┬──────────────────────────────┬───────────────────────┐  ║
║  │ Zone DMA │     Zone NORMAL              │   Zone MOVABLE        │  ║
║  │ 0→16MB   │     16MB→end(ARM32)          │   (CMA, balloon)      │  ║
║  │ 0→1GB    │     1GB→end (ARM64)          │                       │  ║
║  └──────────┴──────────────────────────────┴───────────────────────┘  ║
╚═══════════════════════╦═══════════════════════════════════════════════╝
                        ║ ARM MMU (TTBR1_EL1 / TTBR0_ARM32)
                        ║ Linear map: PA → VA (1:1 offset)
                        ║ MT_NORMAL, Inner Shareable, EL1 R/W
                        ▼
╔═══════════════════════════════════════════════════════════════════════╗
║              BUDDY ALLOCATOR                                          ║
║  ┌─────────────────────────────────────────────────────────────────┐  ║
║  │ free_area[0] (4KB):   ████ ████ ████ ████ ...                   │  ║
║  │ free_area[1] (8KB):   ████████ ████████ ...                     │  ║
║  │ free_area[2] (16KB):  ████████████████ ...                      │  ║
║  │ free_area[3] (32KB):  ████████████████████████████████ ...      │  ║
║  └─────────────────────────────────────────────────────────────────┘  ║
║  struct page[] / vmemmap[] — one entry per 4KB frame                  ║
╚═══════════════════════╦═══════════════════════════════════════════════╝
                        ║ new_slab() → __alloc_pages_node()
                        ║ Returns order-N compound page
                        ▼
╔═══════════════════════════════════════════════════════════════════════╗
║              SLUB ALLOCATOR (kmem_cache_init creates this)            ║
║                                                                       ║
║  ┌─────────────────────────────────────────────────────────────────┐  ║
║  │ kmem_cache_node (per-NUMA-node):                                 │  ║
║  │   node[0]->partial: slab1 → slab2 → slab3 → NULL               │  ║
║  │   node[1]->partial: slab4 → slab5 → NULL         (NUMA)        │  ║
║  │   node[N]->barn: full_sheaves ↔ empty_sheaves                   │  ║
║  └──────────────────────────────┬──────────────────────────────────┘  ║
║                                 │                                       ║
║  ┌──────────────────────────────▼──────────────────────────────────┐  ║
║  │ Per-CPU Sheaves (per-core fast path — NO LOCKS):                 │  ║
║  │  CPU0: [obj_ptr0][obj_ptr1][obj_ptr2]...[obj_ptrN]  (main)      │  ║
║  │  CPU1: [obj_ptr0][obj_ptr1]...[obj_ptrN]            (main)      │  ║
║  │  CPU2: [spare sheaf empty]                                       │  ║
║  │  ARM64: per-cpu data in TPIDR_EL1 or __per_cpu_offset[]         │  ║
║  └──────────────────────────────┬──────────────────────────────────┘  ║
║                                 │                                       ║
║  ┌──────────────────────────────▼──────────────────────────────────┐  ║
║  │ Slab Pages (in linear map, Normal WB-WA):                        │  ║
║  │  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐     │  ║
║  │  │ obj0 │ obj1 │ obj2 │ obj3 │ obj4 │ obj5 │ obj6 │ obj7 │     │  ║
║  │  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘     │  ║
║  │  [struct slab metadata lives in vmemmap, NOT in slab page data]  │  ║
║  └─────────────────────────────────────────────────────────────────┘  ║
╚═══════════════════════╦═══════════════════════════════════════════════╝
                        ║ kmalloc() / kmem_cache_alloc()
                        ▼
╔═══════════════════════════════════════════════════════════════════════╗
║              DRIVER / SUBSYSTEM OBJECTS                               ║
║                                                                       ║
║  task_struct cache → one struct task_struct per process              ║
║  sk_buff cache     → network packet buffers                          ║
║  inode cache       → filesystem inodes                               ║
║  dma_buf cache     → DMA-BUF shared memory handles                  ║
║  gpu_channel cache → GPU command submission channels (driver)        ║
╚═══════════════════════════════════════════════════════════════════════╝
```

### 15.2 ARM32 vs ARM64 Architectural Differences in the Stack

```
ARM32 Specifics:                      ARM64 Specifics:
┌────────────────────────────────┐   ┌────────────────────────────────┐
│ VIPT L1 cache                  │   │ PIPT L1 cache (no aliasing)    │
│ → ARCH_SLAB_MINALIGN=8         │   │ → ARCH_SLAB_MINALIGN=8 (or 16) │
│ → Extra alignment for safety   │   │ → MTE: 16B granule alignment   │
├────────────────────────────────┤   ├────────────────────────────────┤
│ ZONE_HIGHMEM for >760MB        │   │ No HIGHMEM (64-bit VA maps all)│
│ → Slab limited to LOWMEM       │   │ → Slab can use all physical RAM │
├────────────────────────────────┤   ├────────────────────────────────┤
│ LPAE: 40-bit PA                │   │ 48-52 bit PA                   │
│ 3-level page tables            │   │ 4-level page tables (4KB pages) │
├────────────────────────────────┤   ├────────────────────────────────┤
│ mem_map[] global array         │   │ vmemmap at VMEMMAP_START        │
│ (flat, lowmem allocation)      │   │ (sparse-safe, huge-page backed) │
├────────────────────────────────┤   ├────────────────────────────────┤
│ SCU for SMP coherency          │   │ DSU / GIC for SMP coherency    │
│ LDREX/STREX spinlocks + DMB    │   │ LDAXR/STXR (acquire-release)   │
├────────────────────────────────┤   ├────────────────────────────────┤
│ ARCH_DMA_MINALIGN=32-64B       │   │ ARCH_DMA_MINALIGN=128B         │
│ DMA zone = first 16MB          │   │ No DMA zone (IOMMU typical)    │
└────────────────────────────────┘   └────────────────────────────────┘
```

### 15.3 kmalloc Pointer Lifecycle on ARM64

```
1. Driver calls: ptr = kmalloc(64, GFP_KERNEL)
   │
   ▼
2. SLUB fast path (no lock):
   per_cpu sheaf[CPU_N].main->objects[--size]  →  ptr value
   (if sheaf empty, slow path: barn → new slab page → refill sheaf)
   │
   ▼
3. ptr = 0xFFFF800012340040  (linear map VA)
   │
   ▼
4. ARM64 MMU (on access):
   TTBR1_EL1 → PGD[0] → PUD[2] → PMD[9] → PTE[52]
   PTE = PA:0x1234_0040, AttrIndx=3(Normal WB-WA), SH=11(Inner-Shareable), AF=1
   │
   ▼
5. L1 D-cache (PIPT, 64-byte lines):
   PA:0x1234_0040 → tag=0x48D010, set=1, offset=0
   Cache hit (after first access populates line from DRAM)
   │
   ▼
6. Driver writes GPU command into ptr[0..63]:
   Stays in L1/L2 cache; hardware coherency ensures other CPUs see it
   │
   ▼
7. kfree(ptr) on same or different CPU:
   ptr → folio → slab → slab->slab_cache = s
   Push ptr to local per-cpu sheaf (no lock if sheaf not full)
   │
   ▼
8. Sheaf full → exchange with barn (spinlock, batched)
   Eventually slab page returned to buddy if node->nr_partial > min_partial
```
