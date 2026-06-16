# early_ioremap_init() — ARM32 vs ARM64 Detailed Design Comparison
# Architecture-Specific Deep Dive

---

## TABLE OF CONTENTS

1. [Overview: Same Goal, Different Implementation](#1-overview-same-goal-different-implementation)
2. [ARM32 Architecture Background](#2-arm32-architecture-background)
3. [ARM32 Page Table Architecture](#3-arm32-page-table-architecture)
4. [ARM32 early_fixmap_init() Implementation](#4-arm32-early_fixmap_init-implementation)
5. [ARM32 early_ioremap_init() Implementation](#5-arm32-early_ioremap_init-implementation)
6. [ARM32 __set_fixmap() — Hardware Detail](#6-arm32-__set_fixmap---hardware-detail)
7. [ARM32 Memory Layout & Fixmap Region](#7-arm32-memory-layout--fixmap-region)
8. [ARM64 Architecture Background](#8-arm64-architecture-background)
9. [ARM64 Page Table Architecture](#9-arm64-page-table-architecture)
10. [ARM64 early_fixmap_init() Implementation](#10-arm64-early_fixmap_init-implementation)
11. [ARM64 early_ioremap_init() Implementation](#11-arm64-early_ioremap_init-implementation)
12. [ARM64 __set_fixmap() — Hardware Detail](#12-arm64-__set_fixmap---hardware-detail)
13. [ARM64 Memory Layout & Fixmap Region](#13-arm64-memory-layout--fixmap-region)
14. [Side-by-Side Comparison Table](#14-side-by-side-comparison-table)
15. [Critical Differences: SMP TLB Management](#15-critical-differences-smp-tlb-management)
16. [Critical Differences: KASLR Impact on ARM64](#16-critical-differences-kaslr-impact-on-arm64)
17. [Critical Differences: FDT Mapping](#17-critical-differences-fdt-mapping)
18. [Critical Differences: Page Table Levels](#18-critical-differences-page-table-levels)
19. [Boot Sequence Comparison](#19-boot-sequence-comparison)
20. [Code Path Comparison Diagram](#20-code-path-comparison-diagram)

---

## 1. Overview: Same Goal, Different Implementation

Both ARM32 and ARM64 need `early_ioremap_init()` to solve the same boot-time problem: providing virtual address access to physical I/O regions before the full MM subsystem is ready. However, the implementations differ significantly due to:

- ARM32: 32-bit address space (4GB), 2-level page tables, CP15 coprocessor
- ARM64: 64-bit address space, up to 4-level page tables, system registers (MSR/MRS)
- ARM64: KASLR (Kernel Address Space Layout Randomization) complicates fixmap setup
- ARM64: Richer FDT mapping requirements (larger FDT window)
- ARM64: Different TLB management for SMP (late_set_fixmap differs from early)

---

## 2. ARM32 Architecture Background

### 2.1 Address Space

```
ARM32 Virtual Address Space (32-bit, 4GB total):
┌──────────────────────────────────────────────┐ 0xFFFFFFFF
│  Fixmap region                               │ 0xFFC00000 - 0xFFFFFFF
├──────────────────────────────────────────────┤
│  vmalloc / ioremap                           │ 0xF0000000 - 0xFFC00000
├──────────────────────────────────────────────┤
│  Kernel direct-mapped RAM                    │ 0xC0000000 - 0xF0000000
├──────────────────────────────────────────────┤ PAGE_OFFSET = 0xC0000000
│  User space                                  │ 0x00000000 - 0xBFFFFFFF
└──────────────────────────────────────────────┘

Physical Address Space:
- 32-bit (4GB max physical)
- PHYS_OFFSET = 0x80000000 (typical, board-dependent)
- Direct map: virt - PAGE_OFFSET + PHYS_OFFSET = phys
```

### 2.2 MMU and CP15

ARM32 MMU control is through the CP15 coprocessor (System Control Coprocessor):
- `MCR p15, 0, r0, c2, c0, 0` — Write Translation Table Base Register 0 (TTBR0)
- `MCR p15, 0, r0, c2, c0, 1` — Write TTBR1 (kernel page table)
- `MCR p15, 0, r0, c1, c0, 0` — Write System Control Register (enable MMU)
- `MCR p15, 0, r0, c8, c7, 0` — Invalidate all TLBs

ARM32 uses:
- TTBR0 for user space (switchable per process)
- TTBR1 for kernel space (fixed, points to `init_mm.pgd`)

### 2.3 Memory Attributes (ARM32)

ARM32 uses a 4-bit memory type field in PTEs:
```
L_PTE_MT_UNCACHED     = 0x00 << 2  // Device, no cache
L_PTE_MT_BUFFERABLE   = 0x01 << 2  // Device, write-buffer only
L_PTE_MT_WRITETHROUGH = 0x02 << 2  // Normal, write-through cache
L_PTE_MT_WRITEBACK    = 0x03 << 2  // Normal, write-back cache
L_PTE_MT_DEV_SHARED   = 0x04 << 2  // Shared device memory ← used by FIXMAP_PAGE_IO

FIXMAP_PAGE_IO = L_PTE_YOUNG | L_PTE_PRESENT | L_PTE_XN | L_PTE_DIRTY
               | L_PTE_MT_DEV_SHARED | L_PTE_SHARED
```

---

## 3. ARM32 Page Table Architecture

### 3.1 Two-Level Page Table

ARM32 uses a two-level page table:

```
┌─────────────────────────────────────────────────────┐
│              ARM32 Page Table Walk                  │
│                                                     │
│  Virtual Address [31:0]:                            │
│  ┌──────────┬──────────┬────────────────────────┐  │
│  │ PGD idx  │ PTE idx  │   Page Offset          │  │
│  │ [31:21]  │ [20:12]  │   [11:0]               │  │
│  │ 11 bits  │ 9 bits   │   12 bits              │  │
│  │ 2048 ent │ 512 ent  │   4096 bytes/page      │  │
│  └──────────┴──────────┴────────────────────────┘  │
│                                                     │
│  PGD (Page Global Directory):                       │
│  - 4KB file: 2048 entries × 2 bytes                │
│  - First-level descriptor                           │
│  - Covers 2MB per entry                             │
│  - Located at: init_mm.pgd (TTB1)                   │
│                                                     │
│  PTE (Page Table Entry):                            │
│  - 4KB size: 512 entries × 8 bytes (hw+linux pte)  │
│  - Covers 4KB per entry (one page)                  │
│  - For fixmap: stored in bm_pte[] (static array)   │
└─────────────────────────────────────────────────────┘
```

### 3.2 The bm_pte Array

```c
// arch/arm/mm/mmu.c
// bm_pte is statically allocated in BSS — no memory allocator needed!
static pte_t bm_pte[TOTAL_FIX_BTMAPS] __aligned(PAGE_SIZE) __initdata;

// This is 224 × 8 bytes = 1792 bytes (but padded to PAGE_SIZE = 4096 bytes)
// It lives in .init.data and is freed after init
```

The critical design: `bm_pte[]` is **statically declared** in the kernel image. This means it doesn't need any dynamic memory allocation — it exists from the moment the kernel image is loaded. This is what makes early fixmap work without a memory allocator.

---

## 4. ARM32 early_fixmap_init() Implementation

```c
// arch/arm/mm/mmu.c

static pte_t * __init pte_offset_early_fixmap(pmd_t *dir, unsigned long addr)
{
    return &bm_pte[pte_index(addr)];
    // pte_index(addr) = (addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1)
    //                 = bits [20:12] of addr
    // Returns pointer into bm_pte[] for the given virtual address
}

// This function pointer is set during init and switched to late version after paging_init
static pte_t *(*pte_offset_fixmap)(pmd_t *dir, unsigned long addr);

static inline pmd_t * __init fixmap_pmd(unsigned long addr)
{
    return pmd_off_k(addr);
    // Returns the kernel PMD entry for the given virtual address
    // pmd_off_k = pgd_offset_k(addr) traversal → pmd entry
}

void __init early_fixmap_init(void)
{
    pmd_t *pmd;

    // SAFETY CHECK: The early ioremap region must fit within a single PMD
    // (ARM32: each PMD covers 2MB, fixmap region is < 2MB)
    BUILD_BUG_ON((__fix_to_virt(__end_of_early_ioremap_region) >> PMD_SHIFT)
                 != FIXADDR_TOP >> PMD_SHIFT);

    // Get the PMD entry for the top of the fixmap region
    pmd = fixmap_pmd(FIXADDR_TOP);
    
    // Wire up the PMD entry to point to our static bm_pte array
    pmd_populate_kernel(&init_mm, pmd, bm_pte);
    // Writes: PMD[fixmap_index] = physical_address_of(bm_pte) | PMD_TYPE_TABLE
    // Now the hardware can walk: PGD → PMD[fixmap] → bm_pte[] → physical page

    // Switch the PTE accessor to use the early version (no TLB broadcast)
    pte_offset_fixmap = pte_offset_early_fixmap;
}
```

### 4.1 What pmd_populate_kernel() Does

```
Before early_fixmap_init():
PGD entry for 0xFFC00000:  [INVALID / NOT PRESENT]
bm_pte[]:                  [all zeros, no mappings]

After early_fixmap_init():
PGD entry for 0xFFC00000:  [VALID → points to bm_pte[]]
bm_pte[]:                  [still all zeros, but now accessible via hardware walk]

Hardware MMU page walk for 0xFFE00000:
  1. Read TTBR1 → init_mm.pgd base address
  2. PGD[0x7FF] → contains pointer to bm_pte (now valid!)
  3. bm_pte[pte_index(0xFFE00000)] → still zero (unmapped until early_ioremap() is called)
  4. Page fault (if accessed before early_ioremap maps it)
```

---

## 5. ARM32 early_ioremap_init() Implementation

```c
// arch/arm/mm/ioremap.c

/*
 * Must be called after early_fixmap_init
 */
void __init early_ioremap_init(void)
{
    early_ioremap_setup();
}
```

Simple delegation. The ARM32 implementation has no additional work beyond what the generic `early_ioremap_setup()` provides.

### 5.1 Placement in setup_arch()

```c
// arch/arm/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    ...
    early_fixmap_init();      // arch/arm/mm/mmu.c
    early_ioremap_init();     // arch/arm/mm/ioremap.c → early_ioremap_setup()
    parse_early_param();      // Can now use early_ioremap()
    ...
    arm_memblock_init(mdesc);
    adjust_lowmem_bounds();
    early_ioremap_reset();    // mm/early_ioremap.c: after_paging_init = 1
    paging_init(mdesc);       // Full page tables set up
    ...
}
```

---

## 6. ARM32 __set_fixmap() — Hardware Detail

```c
// arch/arm/mm/mmu.c
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot)
{
    unsigned long vaddr = __fix_to_virt(idx);
    // vaddr = FIXADDR_TOP - (idx << PAGE_SHIFT)
    // Converts fixmap index to virtual address

    pte_t *pte = pte_offset_fixmap(pmd_off_k(vaddr), vaddr);
    // Gets the PTE pointer in bm_pte[] corresponding to this vaddr

    // SAFETY CHECKS:
    BUILD_BUG_ON(__fix_to_virt(__end_of_fixed_addresses) < FIXADDR_START);
    BUG_ON(idx >= __end_of_fixed_addresses);

    // ARM32 specific: Before pgprot_kernel is set up, only IO mappings allowed
    if (WARN_ON(pgprot_val(prot) != pgprot_val(FIXMAP_PAGE_IO) &&
                pgprot_val(prot) && pgprot_val(pgprot_kernel) == 0))
        return;

    if (pgprot_val(prot)) {
        // SET MAPPING: Write PTE with physical address and attributes
        set_pte_at(NULL, vaddr, pte,
                   pfn_pte(phys >> PAGE_SHIFT, prot));
        // pfn_pte: constructs PTE = (phys >> PAGE_SHIFT) | prot
        // ARM32 PTE format:
        //   bits [31:12] = physical page frame number
        //   bits [11:0]  = attributes (cache, bufferable, AP bits, etc.)
    } else {
        // CLEAR MAPPING: Remove the PTE
        pte_clear(NULL, vaddr, pte);
    }

    // FLUSH TLB for this virtual address range
    local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE);
    // ARM32 assembly: MCR p15, 0, vaddr, c8, c7, 1  (TLBIMVAIS)
    // Invalidates the TLB entry for this virtual address on the LOCAL CPU only
    // "local" = no IPI to other CPUs (early boot is single-CPU)
}
```

### 6.1 ARM32 PTE Format

```
ARM32 Page Table Entry (32-bit):
┌─────────────────────────────────────────────────┐
│  31          12 │ 11  10 │ 9 │ 8  6 │ 5 │ 4 │ 3 │ 2 │ 1  0 │
├─────────────────┼────────┼───┼──────┼───┼───┼───┼───┼──────┤
│  Physical PFN   │   AP   │ ? │ TEX  │ S │ A │ C │ B │  01  │
│  (phys >> 12)   │ access │   │ type │   │ D │ch │uf │ type │
│                 │ perms  │   │      │shr│iry│e  │fe │      │
└─────────────────┴────────┴───┴──────┴───┴───┴───┴───┴──────┘

For FIXMAP_PAGE_IO:
  AP = 01 (kernel read/write, user no access)
  TEX+CB = Device Shared (non-cacheable, non-bufferable, outer shared)
  S = 1 (shared)
  XN = 1 (execute never - can't execute from device memory)
```

---

## 7. ARM32 Memory Layout & Fixmap Region

```
ARM32 COMPLETE VIRTUAL MEMORY MAP:
                                              (PAGE_SIZE = 4KB)
0xFFFFFFFF ┌─────────────────────────────────────────────┐
           │ (unmapped 1MB gap)                          │
0xFFF00000 ├─────────────────────────────────────────────┤ FIXADDR_TOP
           │ PERMANENT FIXMAP ENTRIES:                   │ (each entry = 4KB)
           │ FIX_EARLYCON_MEM_BASE         4KB           │
           │ FIX_KMAP_BEGIN...END         ~NR_CPUS×32KB  │
           │ FIX_TEXT_POKE0, FIX_TEXT_POKE1   8KB        │
           │                                             │
           ├─────────────────────────────────────────────┤ FIX_BTMAP_END
           │ BOOT-TIME MAP (early_ioremap slots):        │
           │ SLOT 6: indices [193..224]   128KB          │
           │ SLOT 5: indices [161..192]   128KB          │
           │ SLOT 4: indices [129..160]   128KB          │
           │ SLOT 3: indices [97..128]    128KB          │
           │ SLOT 2: indices [65..96]     128KB          │
           │ SLOT 1: indices [33..64]     128KB          │
           │ SLOT 0: indices [1..32]      128KB          │
0xFFC80000 ├─────────────────────────────────────────────┤ FIXADDR_START
           │ (overlap with KMAP region - shared design)  │
0xFFC00000 ├─────────────────────────────────────────────┤
           │                                             │
           │ VMALLOC / IOREMAP:                          │
           │ After paging_init, drivers use ioremap()   │
           │ here to map MMIO registers                  │
           │                                             │
0xF0000000 ├─────────────────────────────────────────────┤ VMALLOC_START (typical)
           │ MODULES:                                    │
           │ Loadable kernel modules mapped here        │
0xBF000000 ├─────────────────────────────────────────────┤
           │ KERNEL DIRECT MAP:                          │
           │ 0xC0000000 ↔ 0x80000000 (phys)             │
           │ Linear mapping, no TLB per page             │
0xC0000000 ├─────────────────────────────────────────────┤ PAGE_OFFSET
           │ USER SPACE                                  │
           │ 0x00000000 to 0xBFFFFFFF                    │
0x00000000 └─────────────────────────────────────────────┘
```

---

## 8. ARM64 Architecture Background

### 8.1 Address Space

ARM64 uses a split virtual address space:

```
ARM64 Virtual Address Space (48-bit or 52-bit VA, Linux uses 48-bit typically):

Kernel space (TTBR1):
0xFFFFFFFFFFFFFFFF ┌──────────────────────────────┐
                   │ Fixmap region                │ ~16MB below top
                   ├──────────────────────────────┤
                   │ vmalloc / ioremap            │
                   ├──────────────────────────────┤
                   │ Linear map (direct-mapped RAM)│
                   ├──────────────────────────────┤
0xFFFF000000000000 └──────────────────────────────┘ KIMAGE_VADDR

User space (TTBR0):
0x0000FFFFFFFFFFFF ┌──────────────────────────────┐
                   │ User mappings                │
0x0000000000000000 └──────────────────────────────┘

VA_BITS = 48 typically (256TB per half)
```

### 8.2 ARM64 System Registers

ARM64 uses MRS/MSR instructions (no coprocessor):
- `TTBR0_EL1` — User space page table base
- `TTBR1_EL1` — Kernel space page table base
- `SCTLR_EL1` — System Control Register (bit 0 = MMU enable)
- `TCR_EL1` — Translation Control Register (page size, VA bits, etc.)
- `MAIR_EL1` — Memory Attribute Indirection Register (8 memory types)

### 8.3 MAIR_EL1 and Memory Attributes

ARM64 uses indirect memory attributes. The MAIR_EL1 register contains 8 attribute entries, and PTEs reference them by 3-bit index:

```
MAIR_EL1 typically configured as:
  Attr0: Device-nGnRnE (most strongly ordered device)
  Attr1: Device-nGnRE  (device, non-gathering, non-reordering, early-write-ack)
  Attr2: Device-GRE    (device, gathering, reordering, early-write-ack)
  Attr3: Normal, non-cacheable
  Attr4: Normal, write-back, read/write allocate (Outer + Inner)
  Attr5-7: Reserved

For FIXMAP_PAGE_IO on ARM64:
  PROT_DEVICE_nGnRE = (PROT_DEFAULT | PTE_PXN | PTE_UXN | PTE_DIRTY |
                       PTE_WRITE | PTE_ATTRINDX(MT_DEVICE_nGnRE))
```

---

## 9. ARM64 Page Table Architecture

### 9.1 Four-Level Page Tables

ARM64 supports up to 4 levels (5 with 52-bit VA):

```
ARM64 Page Table Walk (4KB pages, 48-bit VA):
Virtual Address [47:0]:
┌──────────┬──────────┬──────────┬──────────┬────────────┐
│  PGD idx │  PUD idx │  PMD idx │  PTE idx │ Page Offset│
│  [47:39] │  [38:30] │  [29:21] │  [20:12] │   [11:0]   │
│  9 bits  │  9 bits  │  9 bits  │  9 bits  │  12 bits   │
│  512 ent │  512 ent │  512 ent │  512 ent │  4096 B/pg │
└──────────┴──────────┴──────────┴──────────┴────────────┘

Level 0 (PGD): 512 entries, each covers 512GB
Level 1 (PUD): 512 entries, each covers 1GB
Level 2 (PMD): 512 entries, each covers 2MB
Level 3 (PTE): 512 entries, each covers 4KB

Static pre-allocated arrays for fixmap:
  bm_pte[NR_BM_PTE_TABLES][PTRS_PER_PTE]  in .bss
  bm_pmd[PTRS_PER_PMD]                     in .bss
  bm_pud[PTRS_PER_PUD]                     in .bss
```

### 9.2 NR_FIX_BTMAPS Difference

```c
// ARM32 fixmap.h:
#define NR_FIX_BTMAPS       32    // 32 × 4KB = 128KB per slot

// ARM64 fixmap.h:
#define NR_FIX_BTMAPS       (SZ_256K / PAGE_SIZE)  // 256KB / 4KB = 64 per slot
// 64 × 4KB = 256KB per slot
// Reason: ARM64 FDTs can be much larger, needs bigger mapping window

#define FIX_BTMAPS_SLOTS     7    // Same: 7 slots
// ARM64 total: 7 × 256KB = 1792KB = 1.75MB
```

### 9.3 ARM64 Fixmap enum

```c
// arch/arm64/include/asm/fixmap.h
enum fixed_addresses {
    FIX_HOLE,
    FIX_FDT_END,
    FIX_FDT = FIX_FDT_END + DIV_ROUND_UP(MAX_FDT_SIZE, PAGE_SIZE) + 1,
    // FIX_FDT: A large window just for mapping the Device Tree Blob
    // MAX_FDT_SIZE = 2MB on ARM64 (vs much smaller on ARM32)
    
    FIX_EARLYCON_MEM_BASE,
    FIX_TEXT_POKE0,

    // ACPI/APEI entries (ARM64 supports ACPI, ARM32 typically does not)
    #ifdef CONFIG_ACPI_APEI_GHES
    FIX_APEI_GHES_IRQ,
    FIX_APEI_GHES_SEA,
    #endif

    // Spectre/Meltdown mitigations (ARM64 only)
    #ifdef CONFIG_UNMAP_KERNEL_AT_EL0
    FIX_ENTRY_TRAMP_TEXT1,
    FIX_ENTRY_TRAMP_TEXT2,
    FIX_ENTRY_TRAMP_TEXT3,
    #endif

    __end_of_permanent_fixed_addresses,

    FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
    FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,

    // Extra page table entries (needed because ARM64 uses 4 levels)
    FIX_PTE,
    FIX_PMD,
    FIX_PUD,
    FIX_PGD,

    __end_of_fixed_addresses
};
```

**Key ARM64 additions:**
- `FIX_FDT`: Dedicated large window for DTB mapping (2MB+)
- ACPI GHES entries: ARM64 supports ACPI, needs fixmap slots for hardware error sections
- Trampoline entries: Spectre/Meltdown kernel isolation (KPTI) requires trampoline code pages
- `FIX_PTE/PMD/PUD/PGD`: Temporary page table creation during early boot

---

## 10. ARM64 early_fixmap_init() Implementation

```c
// arch/arm64/mm/fixmap.c

// Static pre-allocated page tables:
static pte_t bm_pte[NR_BM_PTE_TABLES][PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;

// Three-level helper functions (PUD → PMD → PTE):

static void __init early_fixmap_init_pte(pmd_t *pmdp, unsigned long addr)
{
    pmd_t pmd = READ_ONCE(*pmdp);
    pte_t *ptep;

    if (pmd_none(pmd)) {
        ptep = bm_pte[BM_PTE_TABLE_IDX(addr)];
        __pmd_populate(pmdp, __pa_symbol(ptep), PMD_TYPE_TABLE);
        // Write PMD entry: points to correct sub-table in bm_pte[]
        // __pa_symbol() converts kernel symbol to physical address
        // (needed because MMU linear map might not cover bm_pte[] yet)
    }
}

static void __init early_fixmap_init_pmd(pud_t *pudp, unsigned long addr,
                                          unsigned long end)
{
    unsigned long next;
    pud_t pud = READ_ONCE(*pudp);
    pmd_t *pmdp;

    if (pud_none(pud))
        __pud_populate(pudp, __pa_symbol(bm_pmd), PUD_TYPE_TABLE);
    // Wire PUD → bm_pmd[]

    pmdp = pmd_offset_kimg(pudp, addr);
    do {
        next = pmd_addr_end(addr, end);
        early_fixmap_init_pte(pmdp, addr);
        // Wire PMD → bm_pte[subtable]
    } while (pmdp++, addr = next, addr != end);
}

static void __init early_fixmap_init_pud(p4d_t *p4dp, unsigned long addr,
                                          unsigned long end)
{
    p4d_t p4d = READ_ONCE(*p4dp);
    pud_t *pudp;

    if (CONFIG_PGTABLE_LEVELS > 3 && !p4d_none(p4d) &&
        p4d_page_paddr(p4d) != __pa_symbol(bm_pud)) {
        // Special case: 16K pages / 4 levels share PGD entry with kernel mapping
        BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
    }

    if (p4d_none(p4d))
        __p4d_populate(p4dp, __pa_symbol(bm_pud), P4D_TYPE_TABLE);
    // Wire P4D → bm_pud[]

    pudp = pud_offset_kimg(p4dp, addr);
    early_fixmap_init_pmd(pudp, addr, end);
}

void __init early_fixmap_init(void)
{
    unsigned long addr = FIXADDR_TOT_START;  // Start of entire fixmap region
    unsigned long end = FIXADDR_TOP;         // Top of fixmap

    pgd_t *pgdp = pgd_offset_k(addr);
    p4d_t *p4dp = p4d_offset(pgdp, addr);

    early_fixmap_init_pud(p4dp, addr, end);
    // Walks the page table hierarchy and populates all levels with
    // statically allocated tables (bm_pud, bm_pmd, bm_pte[])
}
```

### 10.1 ARM64 vs ARM32 early_fixmap_init() Comparison

```
ARM32: Single PMD entry wired to single bm_pte[]
       (fixmap fits in one 2MB PMD entry)

ARM64: Must wire up potentially multiple levels:
       - One PGD entry for 512GB region
       - PGD → PUD (bm_pud)
       - PUD → PMD (bm_pmd) — may span multiple PMD entries
       - PMD → PTE (bm_pte[0], bm_pte[1], ...) — multiple PTE tables
       
       This is because ARM64 fixmap is much larger (ACPI, FDT, KPTI trampoline)
       and may not fit within a single PMD's 2MB window.
```

---

## 11. ARM64 early_ioremap_init() Implementation

```c
// arch/arm64/mm/ioremap.c

/*
 * Must be called after early_fixmap_init
 */
void __init early_ioremap_init(void)
{
    early_ioremap_setup();
}
```

Structurally identical to ARM32 — just calls `early_ioremap_setup()`. The complexity is entirely in `early_fixmap_init()`.

### 11.1 ARM64 Placement in setup_arch()

```c
// arch/arm64/kernel/setup.c
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
    setup_initial_init_mm(_stext, _etext, _edata, _end);
    *cmdline_p = boot_command_line;

    kaslr_init();    // ARM64 specific: KASLR setup
    
    // KASLR uses non-global mappings from the start if needed
    arm64_use_ng_mappings = kaslr_requires_kpti();

    early_fixmap_init();     // arch/arm64/mm/fixmap.c
    early_ioremap_init();    // arch/arm64/mm/ioremap.c → early_ioremap_setup()

    setup_machine_fdt(__fdt_pointer);  // Uses early_ioremap internally!
    // Difference from ARM32: ARM64 maps FDT here using the early fixmap
    // ARM32 ATAGs/FDT passed via r2, already identity-mapped at this point

    jump_label_init();
    parse_early_param();     // Can use early_ioremap()

    local_daif_restore(DAIF_PROCCTX_NOIRQ);  // ARM64: Unmask async aborts
    ...
    paging_init();           // Full page tables
    ...
}
```

### 11.2 ARM64 Specific: FDT Mapping via early_ioremap

A critical difference: ARM64 actively uses `early_ioremap()` to map the FDT (Device Tree Blob). The FDT physical address comes from `__fdt_pointer` (set by the bootloader in x0 register).

```c
// ARM64 - FDT must be explicitly mapped using early fixmap:
void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    // Uses FIX_FDT fixmap slot (dedicated, large slot for DTB)
    void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
    ...
}
```

For ARM32, the bootloader places FDT at a known physical address, and the identity mapping set up in head.S makes it briefly accessible.

---

## 12. ARM64 __set_fixmap() — Hardware Detail

```c
// arch/arm64/mm/fixmap.c
void __set_fixmap(enum fixed_addresses idx,
                  phys_addr_t phys, pgprot_t flags)
{
    unsigned long addr = __fix_to_virt(idx);
    pte_t *ptep;

    BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

    ptep = fixmap_pte(addr);
    // fixmap_pte() uses: &bm_pte[BM_PTE_TABLE_IDX(addr)][pte_index(addr)]

    if (pgprot_val(flags)) {
        // SET: Write PTE with physical address and attributes
        set_pte(ptep, pfn_pte(phys >> PAGE_SHIFT, flags));
        // ARM64 PTE format: [63:48] upper attrs | [47:12] PFN | [11:0] lower attrs
    } else {
        // CLEAR: Remove PTE
        pte_clear(&init_mm, addr, ptep);
        // Writes zero to PTE entry
        flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
        // ARM64: DSB ISH + TLBI VAAE1IS + DSB ISH + ISB
        // "IS" = Inner Shareable (broadcasts to all CPUs in cluster!)
    }
}
```

### 12.1 ARM64 vs ARM32 TLB Flush Difference

This is a **critical difference**:

```
ARM32 __set_fixmap():
  local_flush_tlb_kernel_range(vaddr, vaddr + PAGE_SIZE)
  └─► MCR p15, 0, vaddr, c8, c7, 1  (TLBIMVAIS — local CPU only)
  
  "local" = single CPU, no IPI needed, fast

ARM64 early (before after_paging_init):
  Uses __early_set_fixmap = __set_fixmap:
    set_pte(ptep, ...) — write PTE only
    No TLB flush for set! (relies on ISB + cache coherency)
    
ARM64 late (after after_paging_init, uses __late_set_fixmap):
  __late_set_fixmap = __set_fixmap:
    pte_clear → flush_tlb_kernel_range:
      DSB ISH          // Ensure PTE write is visible to all CPUs
      TLBI VAAE1IS     // Invalidate by VA, All ASID, EL1, Inner Shareable
                       // Broadcasts to ALL CPUs in the inner shareable domain!
      DSB ISH          // Wait for TLB invalidation to complete on all CPUs
      ISB              // Instruction sync barrier
```

Why the difference? During early boot, there is only one CPU active. After SMP bringup, other CPUs are running and their TLBs must also be invalidated. The "IS" (Inner Shareable) variant of TLBI does this broadcast automatically.

### 12.2 ARM64 PTE Format

```
ARM64 Page Table Entry (64-bit):
┌─────────────────────────────────────────────────────────────────────┐
│ 63      59 │ 58  54 │ 53 │ 52 │ 51     12 │ 11  10 │ 9   8 │ 7     0 │
├────────────┼────────┼────┼────┼───────────┼────────┼───────┼─────────┤
│ Upper attrs│Software│ XN │ PXN│    PFN    │  SH    │ MAIR  │ Lower   │
│ (ignored)  │ bits   │    │    │ [51:12]   │ share  │ index │  attrs  │
└────────────┴────────┴────┴────┴───────────┴────────┴───────┴─────────┘

For FIXMAP_PAGE_IO (PROT_DEVICE_nGnRE):
  PXN = 1  (privileged execute-never)
  UXN = 1  (unprivileged execute-never)
  SH  = 10 (outer shareable)
  MAIR index = 1 → Device-nGnRE (from MAIR_EL1)
  AF  = 1  (accessed flag - set to avoid fault on first access)
  AP  = 01 (kernel read/write, user no access)
  Valid bit = 1
```

---

## 13. ARM64 Memory Layout & Fixmap Region

```
ARM64 KERNEL VIRTUAL MEMORY MAP (48-bit VA, 4KB pages):

0xFFFFFFFFFFFFFFFF ┌─────────────────────────────────────────────┐
                   │ (unmapped guard page)                       │
0xFFFFFFFFFF000000 ├─────────────────────────────────────────────┤ FIXADDR_TOP
                   │ PERMANENT FIXMAP ENTRIES:                   │
                   │  FIX_HOLE                       4KB         │
                   │  FIX_FDT_END                   4KB         │
                   │  FIX_FDT...      (2MB+ window for DTB)      │
                   │  FIX_EARLYCON_MEM_BASE          4KB         │
                   │  FIX_TEXT_POKE0                 4KB         │
                   │  FIX_APEI_GHES_* (ACPI, 4-16KB)            │
                   │  FIX_ENTRY_TRAMP_TEXT1/2/3 (KPTI, 12KB)    │
                   │                                             │
                   ├─────────────────────────────────────────────┤ FIX_BTMAP_END
                   │ BOOT-TIME MAP (early_ioremap slots):        │
                   │  SLOT 6: 64 pages   (256KB)                 │
                   │  SLOT 5: 64 pages   (256KB)                 │
                   │  SLOT 4: 64 pages   (256KB)                 │
                   │  SLOT 3: 64 pages   (256KB)                 │
                   │  SLOT 2: 64 pages   (256KB)                 │
                   │  SLOT 1: 64 pages   (256KB)                 │
                   │  SLOT 0: 64 pages   (256KB)                 │
                   ├─────────────────────────────────────────────┤ FIX_BTMAP_BEGIN
                   │ PAGE TABLE TEMP ENTRIES:                    │
                   │  FIX_PTE, FIX_PMD, FIX_PUD, FIX_PGD        │
                   │  (4 × 4KB = 16KB for early page table work) │
                   ├─────────────────────────────────────────────┤ FIXADDR_TOT_START
                   │                                             │
                   │ VMALLOC / IOREMAP area                      │
                   │ (after paging_init, drivers use ioremap here)│
                   │                                             │
                   ├─────────────────────────────────────────────┤
                   │ LINEAR MAP (direct-mapped physical RAM)      │
                   │ All physical RAM mapped here                │
                   │ phys 0x40000000 → virt 0xFFFF800040000000   │
                   ├─────────────────────────────────────────────┤ PAGE_OFFSET
0xFFFF000000000000 └─────────────────────────────────────────────┘
```

---

## 14. Side-by-Side Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| **Address space** | 32-bit, 4GB | 64-bit, 256TB per half |
| **VA bits** | 32 | 48 (typical), 52 (opt) |
| **PAGE_OFFSET** | 0xC0000000 | 0xFFFF000000000000 |
| **FIXADDR_TOP** | 0xFFF00000 | 0xFFFFFFFFFF000000 (typical) |
| **Page table levels** | 2 (PGD+PTE) | 4 (PGD+PUD+PMD+PTE) |
| **Page size** | 4KB | 4KB, 16KB, or 64KB |
| **NR_FIX_BTMAPS** | 32 (128KB/slot) | 64 (256KB/slot) |
| **FIX_BTMAPS_SLOTS** | 7 | 7 |
| **Max early_ioremap size** | 128KB | 256KB |
| **Total concurrent** | 896KB | 1792KB |
| **MMU control** | CP15 (MCR/MRC) | System regs (MSR/MRS) |
| **TTBR split** | TTBR0/TTBR1 | TTBR0_EL1/TTBR1_EL1 |
| **TLB flush (early)** | TLBIMVAIS (local) | per set_pte, no explicit flush |
| **TLB flush (late)** | TLBIMVAIS (local) | TLBI VAAE1IS (broadcast) |
| **early_fixmap_init** | 1-level setup (PMD→PTE) | 3-level setup (PUD→PMD→PTE) |
| **bm_pte** | Single array, bm_pte[TOTAL] | bm_pte[NR_BM_PTE_TABLES][PTRS_PER_PTE] |
| **FDT mapping** | identity-mapped by bootloader | Must call fixmap_remap_fdt() |
| **KASLR** | Not supported | Yes, affects fixmap addresses |
| **KPTI (Spectre)** | Not supported | FIX_ENTRY_TRAMP_TEXT1/2/3 |
| **ACPI** | Generally no | Yes, FIX_APEI_GHES_* entries |
| **late_set_fixmap** | Same as early | Different: broadcasts TLB inval |
| **Architecture file** | arch/arm/mm/ioremap.c | arch/arm64/mm/ioremap.c |
| **Fixmap setup file** | arch/arm/mm/mmu.c | arch/arm64/mm/fixmap.c |

---

## 15. Critical Differences: SMP TLB Management

### ARM32 SMP TLB

During early boot, ARM32 runs on a single CPU. After SMP bringup:
- Secondary CPUs join with full MMU context
- Fixmap is only manipulated from boot CPU
- `local_flush_tlb_kernel_range()` is sufficient for single-CPU use
- After `paging_init()`, `early_ioremap()` should not be used anyway

### ARM64 SMP TLB

ARM64 uses `TLBI VAAE1IS` (Inner Shareable TLB Invalidate by VA) for late fixmap operations. This is because ARM64 systems commonly have multiple CPUs active earlier:

```c
// ARM64: late_set_fixmap = __set_fixmap uses:
flush_tlb_kernel_range(addr, addr + PAGE_SIZE):
    dsb(ishst);               // Data Synchronization Barrier, Inner Shareable
    __tlbi(vaae1is, addr);    // TLBI VA All ASID EL1 Inner Shareable
    dsb(ish);                 // DSB to ensure completion
    isb();                    // Instruction Synchronization Barrier
```

The `IS` (Inner Shareable) domain includes all cores in a cluster. This ensures ALL CPU caches and TLBs see the updated page table entry, preventing one CPU from using a stale TLB entry that points to old physical memory.

---

## 16. Critical Differences: KASLR Impact on ARM64

ARM64 supports KASLR (Kernel Address Space Layout Randomization). This randomizes the kernel image load address and virtual address at boot time.

```c
// arch/arm64/kernel/setup.c
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
    ...
    kaslr_init();    // Compute KASLR offset from entropy sources
    
    // KASLR with KPTI requires non-global mappings
    // This must be set BEFORE early_fixmap_init() because:
    // the fixmap itself may need to be set up with non-global page table entries
    arm64_use_ng_mappings = kaslr_requires_kpti();
    
    early_fixmap_init();     // Now fixmap is set up with correct NG bit
    early_ioremap_init();
    ...
}
```

The `arm64_use_ng_mappings` flag affects the page table attributes (`PTE_NG` bit — non-global) used in all subsequent page table entries including fixmap entries. This is required to properly implement KPTI (kernel page table isolation) which prevents Spectre/Meltdown attacks.

**ARM32 does not have KASLR**, so `early_fixmap_init()` and `early_ioremap_init()` don't need to consider randomized addresses.

---

## 17. Critical Differences: FDT Mapping

```
ARM32 FDT Access:
  1. Bootloader puts FDT at physical address, passes in r2
  2. head.S creates identity map: phys 0xXXXXXXXX ↔ virt 0xXXXXXXXX
  3. FDT is accessible through identity mapping (no early_ioremap needed)
  4. setup_machine_fdt() can read it directly
  5. Eventually early_ioremap may be used for parts of FDT processing,
     but it's not strictly required for initial access

ARM64 FDT Access:
  1. Bootloader puts FDT at physical address, passes in x0
  2. head.S does NOT create a user-space identity map for FDT
  3. The FDT physical address is saved in __fdt_pointer
  4. After early_ioremap_init():
     setup_machine_fdt() → fixmap_remap_fdt() → uses FIX_FDT fixmap slot
     to create a temporary virtual mapping of the DTB
  5. This is why FIX_FDT is a dedicated, large fixmap slot in ARM64!
```

---

## 18. Critical Differences: Page Table Levels

### ARM32: Two-Level Walk

```
VA [31:0] → PGD index [31:21] → PTE index [20:12] → page offset [11:0]

early_fixmap_init() sets up ONE level:
  PGD[fixmap_index] → bm_pte[]   (single hop)
```

### ARM64: Up to Four-Level Walk

```
VA [47:0] → PGD[47:39] → PUD[38:30] → PMD[29:21] → PTE[20:12] → offset[11:0]

early_fixmap_init() sets up THREE levels:
  PGD[fixmap_pgd_index] → bm_pud[]
  bm_pud[fixmap_pud_index] → bm_pmd[]
  bm_pmd[fixmap_pmd_index] → bm_pte[n][]
  
  (Three levels of static pre-allocation vs one for ARM32)
```

---

## 19. Boot Sequence Comparison

```
ARM32 BOOT SEQUENCE:                    ARM64 BOOT SEQUENCE:
                                        
start_kernel()                          start_kernel()
  └─► setup_arch()                        └─► setup_arch()
        │                                       │
        ├─ setup_processor()                    ├─ kaslr_init()
        ├─ setup_machine_fdt()                  │  (randomize kernel VA)
        │   (ARM32: FDT identity-mapped)         │
        │                                       ├─ arm64_use_ng_mappings = ...
        ├─ early_fixmap_init()                  │  (for KPTI/Spectre)
        │   1 level setup                       │
        │   (PMD → bm_pte)                      ├─ early_fixmap_init()
        │                                       │   3 level setup
        ├─ early_ioremap_init()                 │   (PUD → PMD → PTE)
        │   ← early_ioremap_setup()             │
        │                                       ├─ early_ioremap_init()
        ├─ parse_early_param()                  │   ← early_ioremap_setup()
        │   uses early_ioremap()                │
        │                                       ├─ setup_machine_fdt()
        ├─ early_mm_init()                      │   uses FIX_FDT slot!
        ├─ adjust_lowmem_bounds()               │   (ARM64 MUST map FDT)
        ├─ arm_memblock_init()                  │
        ├─ adjust_lowmem_bounds()               ├─ parse_early_param()
        ├─ early_ioremap_reset()                │
        ├─ paging_init()                        ├─ paging_init()
        └─ kasan_init()                         └─ ...
```

---

## 20. Code Path Comparison Diagram

```
          ARM32                                ARM64
          ─────                                ─────
early_ioremap_init()                  early_ioremap_init()
     │                                     │
     └─► early_ioremap_setup()             └─► early_ioremap_setup()
           │                                     │
           └─► slot_virt[] populated             └─► slot_virt[] populated
               (7 slots, 128KB each)                 (7 slots, 256KB each)


early_ioremap(phys, size)             early_ioremap(phys, size)
     │                                     │
     └─► __early_ioremap()                 └─► __early_ioremap()
           │                                     │
           └─► __early_set_fixmap()              └─► __early_set_fixmap()
                 │                                     │
                 └─► __set_fixmap()                    └─► __set_fixmap()
                       │                                     │
                       ├─ set_pte_at()                       ├─ set_pte()
                       │  [writes to bm_pte]                 │  [writes to bm_pte[n]]
                       │                                     │
                       └─ local_flush_tlb_kernel_range()     └─ (no flush on set)
                          MCR p15 TLBIMVAIS                    flush only on clear:
                          (local CPU only)                      TLBI VAAE1IS
                                                               (all CPUs in cluster)
```

---

*Document created: 2026-05-08*
*Architecture: ARM32 (ARMv7) and ARM64 (AArch64/ARMv8)*
*Kernel Version: Linux mainline*
*Source files referenced:*
- *arch/arm/mm/ioremap.c, arch/arm/mm/mmu.c, arch/arm/include/asm/fixmap.h*
- *arch/arm64/mm/ioremap.c, arch/arm64/mm/fixmap.c, arch/arm64/include/asm/fixmap.h*
- *arch/arm/kernel/setup.c, arch/arm64/kernel/setup.c*
- *mm/early_ioremap.c, include/asm-generic/early_ioremap.h*
