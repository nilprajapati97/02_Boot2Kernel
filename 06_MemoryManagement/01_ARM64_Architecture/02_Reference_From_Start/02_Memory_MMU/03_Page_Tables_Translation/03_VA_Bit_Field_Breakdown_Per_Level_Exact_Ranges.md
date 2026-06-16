# VA Bit Field Breakdown Per Level (Exact Ranges)

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

Every virtual address access on ARM64 is decoded by extracting bit-field slices that index into page tables at each translation level. Understanding the exact bit ranges per level — for each granule size and VA width — is foundational for:
- Debugging TLB and page fault issues
- Writing kernel page table manipulation code
- Understanding why huge pages must be naturally aligned
- Computing page table memory requirements

---

## 2. 4KB Granule VA Bit Fields

### 4KB, 48-bit VA (T0SZ = T1SZ = 16, 4 levels)

```
 63        48 47       39 38       30 29       21 20       12 11        0
  ┌──────────┬──────────┬──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │  PGD[8:0]│  PUD[8:0]│  PMD[8:0]│  PTE[8:0]│  Offset   │
  │  16 bits │  9 bits  │  9 bits  │  9 bits  │  9 bits  │  12 bits  │
  └──────────┴──────────┴──────────┴──────────┴──────────┴────────────┘
  
PGD index  = VA[47:39]  (9 bits → 512 PGD entries)
PUD index  = VA[38:30]  (9 bits → 512 PUD entries)
PMD index  = VA[29:21]  (9 bits → 512 PMD entries)
PTE index  = VA[20:12]  (9 bits → 512 PTE entries)
Page offset= VA[11:0]   (12 bits → 4096 byte pages)
Total: 16 + 9 + 9 + 9 + 9 + 12 = 64 bits ✓
```

### 4KB, 39-bit VA (T0SZ = T1SZ = 25, 3 levels)

When VA width is only 39 bits, the PGD level is folded (collapsed):

```
 63        39 38       30 29       21 20       12 11        0
  ┌──────────┬──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │  PUD[8:0]│  PMD[8:0]│  PTE[8:0]│  Offset   │
  │  25 bits │  9 bits  │  9 bits  │  9 bits  │  12 bits  │
  └──────────┴──────────┴──────────┴──────────┴────────────┘

PUD (=PGD for 3-level) = VA[38:30] (9 bits)
PMD index = VA[29:21]  (9 bits)
PTE index = VA[20:12]  (9 bits)
Page offset = VA[11:0] (12 bits)
Total VA: 39 bits = 9+9+9+12
```

### 4KB, 52-bit VA (LPA2, T0SZ = 12, 5 levels)

```
 63        52 51       48 47       39 38       30 29       21 20       12 11        0
  ┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │  P4D[3:0]│  PGD[8:0]│  PUD[8:0]│  PMD[8:0]│  PTE[8:0]│  Offset  │
  │  12 bits │  4 bits  │  9 bits  │  9 bits  │  9 bits  │  9 bits  │  12 bits │
  └──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴────────────┘

P4D index  = VA[51:48]  (4 bits → 16 entries at level 0)
PGD index  = VA[47:39]  (9 bits → 512 entries)
PUD index  = VA[38:30]  (9 bits)
PMD index  = VA[29:21]  (9 bits)
PTE index  = VA[20:12]  (9 bits)
Page offset= VA[11:0]   (12 bits)
Total: 12 + 4 + 9 + 9 + 9 + 9 + 12 = 64 bits ✓
```

---

## 3. 16KB Granule VA Bit Fields

### 16KB, 48-bit VA (4 levels, 11 bits per level)

```
 63        48 47   47 46       36 35       25 24       14 13        0
  ┌──────────┬─────┬──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │P0[0]│  PGD[10:0]│PUD[10:0] │PMD[10:0] │  Offset   │
  │  16 bits │1 bit│  11 bits  │  11 bits │  11 bits │  14 bits  │
  └──────────┴─────┴──────────┴──────────┴──────────┴────────────┘

Note: ARM64 uses a "concatenated" level 0 for 16KB granule.
The level 0 table has only 2 entries (bit[47] = 0 or 1 selects which half):

Level 0 (concat): VA[47:47]   (1 bit → 2 entries; actual L0 is 2 × 16KB tables)
Level 1 (PGD):    VA[46:36]   (11 bits → 2048 entries)
Level 2 (PUD):    VA[35:25]   (11 bits → 2048 entries)
Level 3 (PMD):    VA[24:14]   (11 bits → 2048 entries, leaf for 16KB pages)
Page offset:      VA[13:0]    (14 bits → 16384 bytes)
Total: 1+11+11+11+14 = 48 bits ✓
```

### 16KB, 36-bit VA (3 levels, common for embedded)

```
 63        36 35       25 24       14 13        0
  ┌──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │  PGD[10:0]│PUD[10:0] │  Offset   │
  │  28 bits │  11 bits  │  11 bits │  14 bits  │
  └──────────┴──────────┴──────────┴────────────┘

PGD index   = VA[35:25]   (11 bits → 2048 entries)
PUD index   = VA[24:14]   (11 bits → 2048 entries, leaf)
Page offset = VA[13:0]    (14 bits)
Total: 28+11+11+14 = 64 bits → 36-bit VA ✓
```

---

## 4. 64KB Granule VA Bit Fields

### 64KB, 48-bit VA (3 levels, 13 bits per level)

```
 63        48 47       42 41       29 28       16 15        0
  ┌──────────┬──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │  P0[5:0] │  PGD[12:0│PMD[12:0] │  Offset   │
  │  16 bits │  6 bits  │  13 bits │  13 bits │  16 bits  │
  └──────────┴──────────┴──────────┴──────────┴────────────┘

Note: Level 0 is "partial" (concatenated) for 64KB granule:
Level 0 (concat): VA[47:42]  (6 bits → 64 entries; L0 tables use only 64 of 8192 slots)
Level 1 (PUD):    VA[41:29]  (13 bits → 8192 entries)
Level 2 (PMD):    VA[28:16]  (13 bits → 8192 entries, leaf for 64KB pages)
Page offset:      VA[15:0]   (16 bits → 65536 bytes)
Total: 6+13+13+16 = 48 bits ✓
```

### 64KB, 42-bit VA (2 levels, common for server)

```
 63        42 41       29 28       16 15        0
  ┌──────────┬──────────┬──────────┬────────────┐
  │ Sign ext │  PGD[12:0]│PMD[12:0] │  Offset   │
  │  22 bits │  13 bits  │  13 bits │  16 bits  │
  └──────────┴──────────┴──────────┴────────────┘

PGD index   = VA[41:29]   (13 bits)
PMD index   = VA[28:16]   (13 bits, leaf)
Page offset = VA[15:0]    (16 bits)
Total: 13+13+16 = 42 bits ✓ (2 levels only!)
```

---

## 5. Block Descriptor Alignment Requirements

Block descriptors map large contiguous physical regions. The alignment requirement is strict:

```
For 4KB granule:
  L2 block (2 MB):  PA must be 2 MB aligned (PA[20:0] = 0)
  L1 block (1 GB):  PA must be 1 GB aligned (PA[29:0] = 0)

For 16KB granule:
  L3 block (16 KB):  PA must be 16 KB aligned (PA[13:0] = 0)
  L2 block (32 MB):  PA must be 32 MB aligned (PA[24:0] = 0)
  L1 block (64 GB):  PA must be 64 GB aligned

For 64KB granule:
  L2 block (64 KB):  PA must be 64 KB aligned (PA[15:0] = 0)
  L1 block (512 MB): PA must be 512 MB aligned
```

---

## 6. Linux Kernel Macros for Bit Field Extraction

```c
// arch/arm64/include/asm/pgtable-hwdef.h

// 4KB granule constants:
#define PAGE_SHIFT      12       // log2(4096)
#define TABLE_SHIFT     9        // bits per level index
#define PTE_INDEX_SIZE  9
#define PMD_INDEX_SIZE  9
#define PUD_INDEX_SIZE  9
#define PGD_INDEX_SIZE  9

#define PAGE_SIZE       (1UL << PAGE_SHIFT)     // 4096
#define TABLE_SIZE      (1UL << (PAGE_SHIFT + TABLE_SHIFT))
                        // not used; each level table = PAGE_SIZE

// Shift values for level boundaries:
#define PTE_SHIFT       PAGE_SHIFT               // 12
#define PMD_SHIFT       (PAGE_SHIFT + TABLE_SHIFT)  // 21
#define PUD_SHIFT       (PMD_SHIFT + TABLE_SHIFT)   // 30
#define PGD_SHIFT       (PUD_SHIFT + TABLE_SHIFT)   // 39

// Extracting index from VA:
#define pgd_index(addr) (((addr) >> PGD_SHIFT) & (PTRS_PER_PGD - 1))
#define pud_index(addr) (((addr) >> PUD_SHIFT) & (PTRS_PER_PUD - 1))
#define pmd_index(addr) (((addr) >> PMD_SHIFT) & (PTRS_PER_PMD - 1))
#define pte_index(addr) (((addr) >> PTE_SHIFT) & (PTRS_PER_PTE - 1))

// PTRS_PER_x: number of entries in each level table
#define PTRS_PER_PGD    512  // 2^9
#define PTRS_PER_PUD    512
#define PTRS_PER_PMD    512
#define PTRS_PER_PTE    512

// Size covered by one entry at each level:
#define PTE_SIZE        PAGE_SIZE           // 4 KB
#define PMD_SIZE        (1UL << PMD_SHIFT)  // 2 MB
#define PUD_SIZE        (1UL << PUD_SHIFT)  // 1 GB
#define PGD_SIZE        (1UL << PGD_SHIFT)  // 512 GB
```

---

## 7. Finding the Level From VA Alignment

Quick rule: a VA/PA is N-level aligned if its lower (level_shift) bits are zero:

```
PA is 4KB-aligned?   PA & ((1<<12)-1) == 0  → can use page descriptor
PA is 2MB-aligned?   PA & ((1<<21)-1) == 0  → can use L2 block
PA is 1GB-aligned?   PA & ((1<<30)-1) == 0  → can use L1 block
```

This is how the kernel decides whether to create a huge page (block) or must split into individual PTEs:

```c
// arch/arm64/mm/mmu.c
static void alloc_init_pmd(...)
{
    unsigned long next = pmd_addr_end(addr, end);
    // Can we use a 2MB block here?
    if (((addr | next | phys) & ~PMD_MASK) == 0) {
        // All naturally aligned → use block descriptor
        set_pmd(pmd, pfn_pmd(pfn, prot));
    } else {
        // Not aligned → must use PTE level
        alloc_init_pte(pmd, addr, next, pfn, prot);
    }
}
```

---

## 8. Interview Questions & Answers

**Q1: For a 48-bit VA with 4KB pages, what are the exact bit ranges used to index each page table level?**

Starting from bit 47 and working down:
- L0 (PGD): bits[47:39] (9 bits, 512 entries)
- L1 (PUD): bits[38:30] (9 bits, 512 entries)
- L2 (PMD): bits[29:21] (9 bits, 512 entries)
- L3 (PTE): bits[20:12] (9 bits, 512 entries)
- Page offset: bits[11:0] (12 bits)

Each level has exactly 9 bits of index because 4KB / 8 bytes per entry = 512 = 2^9 entries per table.

**Q2: Why must a 2MB huge page be naturally aligned?**

A 2MB block descriptor at L2 (PMD level) maps VA[29:21] to a single physical block. The physical block must have PA[20:0] = 0 (2MB-aligned) because the block descriptor doesn't store PA[20:0] — those bits are implicit zeros. If the PA were not 2MB-aligned, the block mapping would be incorrect (the hardware uses PA[47:21] from the descriptor concatenated with VA[20:0] for the final PA). This is why `mmap(MAP_HUGETLB)` requires 2MB-aligned memory, and why transparent huge pages (THP) promotion fails if a page is not 2MB-aligned.

**Q3: How does the 39-bit VA configuration reduce page table levels?**

With T0SZ=25 (39-bit VA), the highest 25 bits are sign extension. The remaining 39 VA bits fit into only 3 index levels plus the page offset: 9+9+9+12=39. The PGD (L0) level is folded/collapsed — Linux treats the PUD as if it were the PGD. `pgd_offset()` returns a PUD entry, not an L0 entry. This is the "folded page table" concept: Linux's generic code uses 4-level terminology but collapses unused levels into the next one, keeping code generic.

---

## 9. Quick Reference: Bit Ranges Summary

| Granule | VA Bits | L0 bits | L1 bits | L2 bits | L3 bits | Offset |
|---|---|---|---|---|---|---|
| 4KB | 48 | [47:39] | [38:30] | [29:21] | [20:12] | [11:0] |
| 4KB | 39 | folded | [38:30] | [29:21] | [20:12] | [11:0] |
| 16KB | 48 | [47:47] | [46:36] | [35:25] | [24:14] | [13:0] |
| 16KB | 36 | folded | folded | [35:25] | [24:14] | [13:0] |
| 64KB | 48 | [47:42] | [41:29] | [28:16] | leaf | [15:0] |
| 64KB | 42 | folded | [41:29] | [28:16] | leaf | [15:0] |

| Level | Shift (4KB) | Coverage | Block size |
|---|---|---|---|
| PTE (L3) | 12 | 4 KB | N/A (leaf) |
| PMD (L2) | 21 | 2 MB | 2 MB block |
| PUD (L1) | 30 | 1 GB | 1 GB block |
| PGD (L0) | 39 | 512 GB | — |
