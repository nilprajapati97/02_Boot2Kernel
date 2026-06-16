# 05 — Page Table Chain: `__create_pgd_mapping()` Walkthrough

[← Doc 04: Memblock Operations](04_Memblock_Operations.md) | **Doc 05** | [Doc 06: Zone, Node & Page Init →](06_Zone_Node_Page_Init.md)

---

## Overview

When `paging_init()` calls `map_mem()`, every physical memory region is mapped into the
kernel linear map. This document traces the **exact** page table construction chain:
`__create_pgd_mapping()` → `alloc_init_pud()` → `alloc_init_cont_pmd()` →
`init_pmd()` → `alloc_init_cont_pte()` → `init_pte()`.

---

## 1. Entry Point: `__create_pgd_mapping()`

**Source**: `arch/arm64/mm/mmu.c`  
**Called by**: `__map_memblock()` via `early_create_pgd_mapping()`  
**Prototype**:

```c
static void __create_pgd_mapping(
    pgd_t *pgdir,                 // PGD base (swapper_pg_dir)
    phys_addr_t phys,             // Physical address to map
    unsigned long virt,           // Virtual address target
    phys_addr_t size,             // Size of mapping
    pgprot_t prot,                // Protection attributes
    phys_addr_t (*pgtable_alloc)(int),  // Page allocator function
    int flags)                    // NO_EXEC_MAPPINGS, NO_BLOCK_MAPPINGS, etc.
```

**Code**:

```c
static void __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
                                  unsigned long virt, phys_addr_t size,
                                  pgprot_t prot,
                                  phys_addr_t (*pgtable_alloc)(int),
                                  int flags)
{
    unsigned long addr, end, next;
    pgd_t *pgdp = pgd_offset_pgd(pgdir, virt);
    //   pgdp = &pgdir[pgd_index(virt)]
    //   pgd_index(virt) = (virt >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1)
    //   PGDIR_SHIFT = 39 (4-level, 4KB granule)

    phys &= PAGE_MASK;           // Align PA to page boundary
    addr = virt & PAGE_MASK;     // Align VA to page boundary
    end = PAGE_ALIGN(virt + size);  // Round up end

    do {
        next = pgd_addr_end(addr, end);
        //   next = min(addr aligned to next PGD boundary, end)
        //   PGD boundary = addr & ~(PGDIR_SIZE - 1) + PGDIR_SIZE
        //   PGDIR_SIZE = 512 GB (for 4-level, 4KB granule)

        alloc_init_pud(pgdp, addr, next, phys, prot, pgtable_alloc, flags);
        //   Handle this PGD entry's range → recurse into PUD

        phys += next - addr;     // Advance PA
    } while (pgdp++, addr = next, addr != end);
}
```

**PGD entry options**:
- If PGD entry is empty → allocate a new PUD page, install table descriptor
- If PGD entry exists → follow it to existing PUD

---

## 2. `alloc_init_pud()` — PUD Level

**Source**: `arch/arm64/mm/mmu.c`

```c
static void alloc_init_pud(pgd_t *pgdp, unsigned long addr, unsigned long end,
                           phys_addr_t phys, pgprot_t prot,
                           phys_addr_t (*pgtable_alloc)(int),
                           int flags)
{
    p4d_t *p4dp = p4d_offset_kimg(pgdp, addr);
    //   For 3-level or 4-level PT with CONFIG_PGTABLE_LEVELS >= 4:
    //   p4dp = &p4d_table[p4d_index(addr)]
    //   (For ARM64 with 4KB pages: P4D is folded into PGD, so p4dp = pgdp)

    pud_t *pudp, *old_pudp;
    phys_addr_t pud_phys;
    unsigned long next;

    if (p4d_none(READ_ONCE(*p4dp))) {
        /* PUD page doesn't exist → allocate one */
        pud_phys = pgtable_alloc(PUD_SHIFT);
        //   pgtable_alloc = early_pgtable_alloc
        //   → memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE)
        //   Returns PA of zeroed page

        __p4d_populate(p4dp, pud_phys, P4D_TYPE_TABLE);
        //   *p4dp = pud_phys | PMD_TYPE_TABLE | PMD_TABLE_PXN
        //   Now PGD→P4D→PUD chain exists
    }
    BUG_ON(p4d_bad(READ_ONCE(*p4dp)));

    pudp = pud_offset_kimg(p4dp, addr);
    //   pudp = &pud_table[pud_index(addr)]
    //   pud_index = (addr >> PUD_SHIFT) & (PTRS_PER_PUD - 1)
    //   PUD_SHIFT = 30 (1 GB per entry)

    do {
        pud_t old_pud = READ_ONCE(*pudp);

        next = pud_addr_end(addr, end);
        //   next = min(addr aligned to next PUD boundary, end)
        //   PUD boundary = 1 GB

        /* ─── Can we use a 1GB block mapping? ─── */
        if (use_1G_block(addr, next, phys) &&
            (flags & NO_BLOCK_MAPPINGS) == 0) {
            pud_set_huge(pudp, phys, prot);
            //   *pudp = phys | PUD_TYPE_SECT | prot
            //   SINGLE entry maps 1 GB (no PMD/PTE needed)
        } else {
            alloc_init_cont_pmd(pudp, addr, next, phys, prot,
                                pgtable_alloc, flags);
            //   Need finer mapping → descend to PMD level
        }

        phys += next - addr;
    } while (pudp++, addr = next, addr != end);
}
```

### 2.1 `use_1G_block()` — 1GB Block Decision

```c
static bool use_1G_block(unsigned long addr, unsigned long next, phys_addr_t phys)
{
    if (PAGE_SHIFT != 12)
        return false;                    // Only for 4KB pages
    if (!IS_ENABLED(CONFIG_ARM64_4K_PAGES))
        return false;
    if (next - addr < PUD_SIZE)
        return false;                    // Range < 1GB
    if (!IS_ALIGNED(phys, PUD_SIZE))
        return false;                    // PA not 1GB-aligned
    return true;
}
```

**Result**: 1GB blocks when PA and VA span ≥ 1GB and are 1GB-aligned. Otherwise → PMD.

---

## 3. `alloc_init_cont_pmd()` → `init_pmd()` — PMD Level

**Source**: `arch/arm64/mm/mmu.c`

```c
static void alloc_init_cont_pmd(pud_t *pudp, unsigned long addr,
                                unsigned long end, phys_addr_t phys,
                                pgprot_t prot,
                                phys_addr_t (*pgtable_alloc)(int),
                                int flags)
{
    unsigned long next;
    pud_t pud = READ_ONCE(*pudp);

    /* Ensure we have a PMD table under this PUD entry */
    if (pud_none(pud)) {
        phys_addr_t pmd_phys = pgtable_alloc(PMD_SHIFT);
        //   Allocate PMD page from memblock
        __pud_populate(pudp, pmd_phys, PUD_TYPE_TABLE);
        pud = READ_ONCE(*pudp);
    }
    BUG_ON(pud_bad(pud));

    do {
        pgprot_t __prot = prot;

        next = pmd_cont_addr_end(addr, end);
        //   Align to contiguous PMD boundary (CONT_PMD_SIZE = 32 MB for 4KB pages)
        //   A contiguous hint group = 16 PMD entries = 16 × 2MB = 32 MB

        /* Set PTE_CONT if the entire contiguous range is within our mapping */
        if ((flags & NO_CONT_MAPPINGS) == 0 &&
            (addr | next | phys) % CONT_PMD_SIZE == 0) {
            __prot = __pgprot(pgprot_val(prot) | PTE_CONT);
        }

        init_pmd(pudp, addr, next, phys, __prot, pgtable_alloc, flags);
        phys += next - addr;
    } while (addr = next, addr != end);
}
```

### 3.1 `init_pmd()` — 2MB Section vs 4KB Pages Decision

```c
static void init_pmd(pud_t *pudp, unsigned long addr, unsigned long end,
                     phys_addr_t phys, pgprot_t prot,
                     phys_addr_t (*pgtable_alloc)(int),
                     int flags)
{
    unsigned long next;
    pmd_t *pmdp = pmd_offset_kimg(pudp, addr);
    //   pmdp = &pmd_table[pmd_index(addr)]
    //   pmd_index = (addr >> PMD_SHIFT) & (PTRS_PER_PMD - 1)
    //   PMD_SHIFT = 21 (2 MB per entry)

    do {
        pmd_t old_pmd = READ_ONCE(*pmdp);

        next = pmd_addr_end(addr, end);
        //   next = min(addr + 2MB aligned, end)

        /* ─── Can we use a 2MB section mapping? ─── */
        if (((addr | next | phys) & ~PMD_MASK) == 0 &&
            (flags & NO_BLOCK_MAPPINGS) == 0) {
            pmd_set_huge(pmdp, phys, prot);
            //   *pmdp = phys | PMD_TYPE_SECT | prot
            //   SINGLE entry maps 2 MB → no PTE table needed

        } else {
            alloc_init_cont_pte(pmdp, addr, next, phys, prot,
                                pgtable_alloc, flags);
            //   Not 2MB-aligned or block mappings disabled
            //   → descend to PTE level
        }

        phys += next - addr;
    } while (pmdp++, addr = next, addr != end);
}
```

**Section mapping decision**: `(addr | next | phys) & ~PMD_MASK == 0` checks that
all of: start VA, end VA, and PA are 2MB-aligned.

---

## 4. `alloc_init_cont_pte()` → `init_pte()` — PTE Level

**Source**: `arch/arm64/mm/mmu.c`

```c
static void alloc_init_cont_pte(pmd_t *pmdp, unsigned long addr,
                                unsigned long end, phys_addr_t phys,
                                pgprot_t prot,
                                phys_addr_t (*pgtable_alloc)(int),
                                int flags)
{
    unsigned long next;
    pmd_t pmd = READ_ONCE(*pmdp);

    if (pmd_none(pmd)) {
        phys_addr_t pte_phys = pgtable_alloc(PAGE_SHIFT);
        //   Allocate PTE page from memblock (512 × 8-byte entries)
        __pmd_populate(pmdp, pte_phys, PMD_TYPE_TABLE);
        pmd = READ_ONCE(*pmdp);
    }
    BUG_ON(pmd_bad(pmd));

    do {
        pgprot_t __prot = prot;

        next = pte_cont_addr_end(addr, end);
        //   CONT_PTE_SIZE = 64 KB (16 × 4KB pages)
        //   Contiguous hint: 16 PTEs can be cached as one TLB entry

        if ((flags & NO_CONT_MAPPINGS) == 0 &&
            (addr | next | phys) % CONT_PTE_SIZE == 0) {
            __prot = __pgprot(pgprot_val(prot) | PTE_CONT);
        }

        init_pte(pmdp, addr, next, phys, __prot);
        phys += next - addr;
    } while (addr = next, addr != end);
}
```

### 4.1 `init_pte()` — Writing Individual PTEs

```c
static void init_pte(pmd_t *pmdp, unsigned long addr, unsigned long end,
                     phys_addr_t phys, pgprot_t prot)
{
    pte_t *ptep = pte_offset_kimg(pmdp, addr);
    //   ptep = &pte_table[pte_index(addr)]
    //   pte_index = (addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1)
    //   PAGE_SHIFT = 12

    do {
        pte_t old_pte = READ_ONCE(*ptep);

        set_pte(ptep, pfn_pte(__phys_to_pfn(phys), prot));
        //   *ptep = (phys >> PAGE_SHIFT) << 12 | pgprot_val(prot) | PTE_TYPE_PAGE
        //   This is the FINAL hardware page table entry

        phys += PAGE_SIZE;
    } while (ptep++, addr += PAGE_SIZE, addr != end);
}
```

**PTE format** (AArch64 Stage 1, 4KB granule):

```
Bits [63:52] → Upper attributes (UXN, PXN, Contiguous, etc.)
Bits [51:48] → Reserved / Output address [51:48]
Bits [47:12] → Output address [47:12] (PA of the 4KB page)
Bits [11:10] → Access Permissions (AP[2:1])
Bits [9:8]   → Shareability (SH)
Bits [7:6]   → Reserved
Bits [5]     → Access Flag (AF)
Bits [4:2]   → AttrIndx (index into MAIR_EL1)
Bits [1:0]   → Descriptor type (0b11 = page)
```

---

## 5. The Page Table Allocator: `early_pgtable_alloc()`

**Source**: `arch/arm64/mm/mmu.c`

```c
static phys_addr_t __init early_pgtable_alloc(int shift)
{
    phys_addr_t phys;
    void *ptr;

    phys = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
    //   Allocate one page (4096 bytes) aligned to page boundary
    //   From memblock (top-down by default)

    if (!phys)
        panic("Failed to allocate page table page\n");

    ptr = __va(phys);               // Get virtual address (via linear map)
    memset(ptr, 0, PAGE_SIZE);      // Zero the page (invalid entries)

    return phys;                    // Return physical address
}
```

---

## 6. Complete Call Chain for Mapping a Physical Region

Example: mapping 2GB RAM from PA 0x4000_0000 to linear map:

```
paging_init()
└── map_mem(swapper_pg_dir)
    └── __map_memblock(swapper_pg_dir, 0x4000_0000, 0xC000_0000, PAGE_KERNEL, flags)
        └── __create_pgd_mapping(swapper_pg_dir, 0x4000_0000, __va(0x4000_0000), 
                                 0x8000_0000, PAGE_KERNEL, early_pgtable_alloc, flags)
            │
            │  PGD: index = (__va(0x4000_0000) >> 39) & 0x1FF
            │
            └── alloc_init_pud(pgdp, addr, next, phys, ...)
                │
                │  PUD entry for [0x4000_0000 .. 0x7FFF_FFFF] (1GB):
                │  0x4000_0000 IS 1GB-aligned → 1GB BLOCK MAP ✓
                │  *pudp = 0x4000_0000 | PUD_TYPE_SECT | prot
                │
                │  PUD entry for [0x8000_0000 .. 0xBFFF_FFFF] (1GB):
                │  0x8000_0000 IS 1GB-aligned → 1GB BLOCK MAP ✓
                │  *pudp = 0x8000_0000 | PUD_TYPE_SECT | prot
                │
                │  (2 × 1GB blocks map the entire 2GB range)
                │
                │  ── If region were NOT 1GB-aligned: ──
                │
                ├── alloc_init_cont_pmd(pudp, ...)
                │   └── init_pmd(pudp, ...)
                │       │
                │       │  2MB-aligned chunks → PMD_TYPE_SECT
                │       │  Unaligned edges:
                │       │
                │       └── alloc_init_cont_pte(pmdp, ...)
                │           └── init_pte(pmdp, ...)
                │               │  4KB page entries
                │               │  *ptep = phys | PTE_TYPE_PAGE | prot
                │               └── (one PTE per 4KB page)
```

---

## 7. Protection Values at Each Level

| Level | Block/Table | Key Protection Bits |
|-------|-------------|---------------------|
| PGD→P4D | Table descriptor | `PMD_TYPE_TABLE \| PMD_TABLE_PXN` |
| PUD | 1GB block | `PUD_TYPE_SECT \| AP_KERN_RW \| AttrIdx(MT_NORMAL) \| AF \| SH_ISH \| UXN \| PXN` |
| PUD | Table to PMD | `PMD_TYPE_TABLE \| PMD_TABLE_PXN` |
| PMD | 2MB section | `PMD_TYPE_SECT \| AP_KERN_RW \| AttrIdx(MT_NORMAL) \| AF \| SH_ISH \| UXN \| PXN` |
| PMD | Table to PTE | `PMD_TYPE_TABLE` |
| PTE | 4KB page | `PTE_TYPE_PAGE \| AP_KERN_RW \| AttrIdx(MT_NORMAL) \| AF \| SH_ISH \| UXN \| PXN` |

For linear map (`PAGE_KERNEL`):
- **AttrIdx** = 3 → `MT_NORMAL` (Write-Back, Read/Write Allocate)
- **SH** = Inner Shareable (for SMP coherency)
- **AP** = Kernel RW
- **UXN + PXN** = 1 → No execute (linear map is NX)
- **AF** = Access Flag set (avoid access fault)

---

## State After Document 05

```
┌──────────────────────────────────────────────────────────┐
│  Page Table Construction Complete:                       │
│                                                          │
│  swapper_pg_dir hierarchy:                               │
│    PGD (512 × 8 bytes = 4KB)                             │
│     └─ PUD (1GB entries)                                 │
│         ├─ 1GB blocks: if PA+VA are 1GB-aligned          │
│         └─ PMD table:                                    │
│             ├─ 2MB sections: if PA+VA 2MB-aligned         │
│             └─ PTE table:                                │
│                 └─ 4KB pages: unaligned edges             │
│                                                          │
│  Allocator chain:                                        │
│    __create_pgd_mapping                                  │
│      → alloc_init_pud                                    │
│        → alloc_init_cont_pmd → init_pmd                  │
│          → alloc_init_cont_pte → init_pte                │
│                                                          │
│  Page table pages allocated from:                        │
│    early_pgtable_alloc → memblock_phys_alloc             │
│    (top-down, from high physical addresses)              │
│                                                          │
│  Optimization: contiguous hint (PTE_CONT)                │
│    16 × 2MB = 32MB at PMD level                          │
│    16 × 4KB = 64KB at PTE level                          │
│    Hardware caches as single TLB entry                   │
│                                                          │
│  Next: Zone/Node/Page init (free_area_init)              │
└──────────────────────────────────────────────────────────┘
```

---

[← Doc 04: Memblock Operations](04_Memblock_Operations.md) | **Doc 05** | [Doc 06: Zone, Node & Page Init →](06_Zone_Node_Page_Init.md)
