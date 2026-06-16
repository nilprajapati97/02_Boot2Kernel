# Four-Level Page Table: PGD, PUD, PMD, PTE (4KB Granule)

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

ARM64 with 4KB granule and 48-bit VA uses a four-level page table hierarchy. Each level narrows down the translation by 9 bits, ultimately pointing to a 4KB physical page.

The four levels are:
- **PGD** (Page Global Directory) — Level 0 in ARM64 (L0)
- **PUD** (Page Upper Directory) — Level 1 (L1)
- **PMD** (Page Middle Directory) — Level 2 (L2)
- **PTE** (Page Table Entry) — Level 3 (L3), the leaf level

Linux uses these names for portability. ARM architecture documentation calls them: L0, L1, L2, L3 or (using the full translation terminology) "translation table level 0–3".

---

## 2. VA Bit Field Breakdown (4KB Granule, 48-bit VA)

```
VA bit range:   [47] [46...39] [38...30] [29...21] [20...12] [11...0]
                                                                       
Field:          Sign  PGD idx   PUD idx   PMD idx   PTE idx   Page offset
Bits:            1      9         9         9         9          12
Level:                  L0        L1        L2        L3        Physical page

Sign bit[47]: 
  0 → use TTBR0_EL1 (user space)
  1 → use TTBR1_EL1 (kernel space)
```

```
Full 48-bit VA breakdown example:
VA = 0x0000_1234_5678_9ABC

Bit[47] = 0 → TTBR0 (user space)
PGD index = VA[47:39] = bits 39–47 → 0x00 (= 0, so entry 0 of PGD)
PUD index = VA[38:30] = 0x048 (= decimal 72)
PMD index = VA[29:21] = 0x0D1 (= decimal 209)
PTE index = VA[20:12] = 0x178 (= decimal 376)
Page offset = VA[11:0] = 0xABC
```

---

## 3. Page Table Entry Count and Size

```
Each page table is exactly one 4KB page:
  512 entries × 8 bytes = 4096 bytes = 1 page

TTBR0_EL1 or TTBR1_EL1:
  Points to the PGD (L0) physical base address (4KB-aligned)

PGD[PGD_idx] → points to PUD (L1) base
PUD[PUD_idx] → points to PMD (L2) base OR 1 GB block
PMD[PMD_idx] → points to PTE (L3) base OR 2 MB block
PTE[PTE_idx] → points to 4 KB physical page (plus attributes)
```

---

## 4. Full Translation Walk (Step by Step)

### Input: VA = 0xFFFF_0001_2345_6789 (kernel linear map example)

Step 1: Determine TTBR
```
VA[63:48] = 0xFFFF → TTBR1_EL1 = kernel page tables
```

Step 2: Extract indices
```
VA = 0xFFFF_0001_2345_6789
        ^^^^^^^^           → bits[63:48] = sign extension (TTBR1)
                ^^^        → PGD bits[47:39] = 0x0 (shifted: VA bits 39-47 = 0x00)
                             Actually: 0xFFFF_0001_2345_6789
                             
Let's do properly:
Numeric VA: 0xFFFF000123456789

Binary: 1111 1111 1111 1111 | 0000 0000 0000 0001 | 0010 0011 0100 0101 | 0110 0111 1000 1001

VA[47:39] = 000 0000 00 = 0x000 → PGD index = 0
VA[38:30] = 0 0000 0001 = 0x001 → PUD index = 1
VA[29:21] = 0010 0011 0 = 0x046 → PMD index = 70
VA[20:12] = 1001 0000 1 = 0x091 → Wait, let me redo

For clarity, use a simpler example:
VA = 0xFFFF_0000_0020_1ABC (kernel linear map)

bits[47:39] = 0b000_000_000 = 0  → PGD index 0
bits[38:30] = 0b000_000_000 = 0  → PUD index 0
bits[29:21] = 0b000_010_000 = 16 → PMD index 16
bits[20:12] = 0b000_000_001 = 1  → PTE index 1
bits[11:0]  = 0xABC           → Page offset 0xABC
```

Step 3: PGD Lookup
```
TTBR1_EL1 = 0x0000_4000_0000_0000  (kernel swapper_pg_dir physical address)
PGD entry address = TTBR1_base + PGD_idx × 8
                  = 0x4000_0000_0000 + 0 × 8
                  = 0x4000_0000_0000

Read PGD[0] = 0x0000_4000_0010_0003  (example)
             = Table descriptor: points to PUD at PA = 0x4000_0010_0000
             bits[1:0] = 0b11 → table descriptor (points to next level)
             bits[47:12] = next level table PA >> 12
```

Step 4: PUD Lookup
```
PUD base = 0x4000_0010_0000
PUD entry address = 0x4000_0010_0000 + 0 × 8 = 0x4000_0010_0000
Read PUD[0] = 0x0000_4000_0020_0003  → points to PMD at PA = 0x4000_0020_0000
```

Step 5: PMD Lookup
```
PMD base = 0x4000_0020_0000
PMD entry address = 0x4000_0020_0000 + 16 × 8 = 0x4000_0020_0080
Read PMD[16] = 0x0000_4000_0030_0003  → points to PTE at PA = 0x4000_0030_0000
(Could also be a 2MB block: bits[1:0] = 0b01, not 0b11)
```

Step 6: PTE Lookup
```
PTE base = 0x4000_0030_0000
PTE entry address = 0x4000_0030_0000 + 1 × 8 = 0x4000_0030_0008
Read PTE[1] = 0x0040_0000_00A0_0703
  → bits[1:0] = 0b11 → valid page descriptor
  → PA[47:12] = 0x00A0_000  → Physical page at 0x00A0_000 × 4KB = 0xA00_0000
  → Attributes: RW, Normal memory, Inner Shareable

Final PA = PTE.PA[47:12] << 12 | VA[11:0]
         = 0x00A0_0000 | 0xABC
         = 0x00A0_0ABC
```

---

## 5. Linux Kernel Page Table Types

```c
// arch/arm64/include/asm/pgtable-types.h

typedef u64  pteval_t;    // Raw PTE value type
typedef u64  pmdval_t;    // Raw PMD value type
typedef u64  pudval_t;    // Raw PUD value type
typedef u64  p4dval_t;    // Raw P4D value type (not used with 4 levels)
typedef u64  pgdval_t;    // Raw PGD value type

// Wrapper types for type safety:
typedef struct { pteval_t pte; } pte_t;
typedef struct { pmdval_t pmd; } pmd_t;
typedef struct { pudval_t pud; } pud_t;
typedef struct { pgdval_t pgd; } pgd_t;
typedef struct { pteval_t pgprot; } pgprot_t;
```

### Accessor Macros

```c
// include/linux/pgtable.h + arch/arm64/include/asm/pgtable.h

// Walk the page tables:
pgd_t *pgd_offset(struct mm_struct *mm, unsigned long addr)
    → Returns pointer to PGD entry for addr in mm

pud_t *pud_offset(p4d_t *p4d, unsigned long addr)
    → Returns pointer to PUD entry

pmd_t *pmd_offset(pud_t *pud, unsigned long addr)
    → Returns pointer to PMD entry

pte_t *pte_offset_kernel(pmd_t *pmd, unsigned long addr)
    → Returns pointer to PTE entry (kernel linear map)

// Check entry validity:
int pgd_none(pgd_t pgd)  → 1 if entry is invalid/empty
int pud_none(pud_t pud)
int pmd_none(pmd_t pmd)
int pte_none(pte_t pte)

// Extract PA from entry:
phys_addr_t pgd_page_paddr(pgd_t pgd)
phys_addr_t pmd_page_paddr(pmd_t pmd)
unsigned long pte_pfn(pte_t pte)
```

---

## 6. Kernel Swapper PGD vs Process PGD

```c
// The kernel has a global page table: swapper_pg_dir
// arch/arm64/kernel/vmlinux.lds.S
// .pg_dir : { ... }  → swapper_pg_dir symbol

// Process mm_struct points to its own PGD:
struct mm_struct {
    pgd_t *pgd;         // Process's PGD (allocated from page allocator)
    ...
};

// Kernel thread's mm:
// init_mm.pgd = swapper_pg_dir

// On context switch:
// If switching to user process: write process->mm->pgd + ASID to TTBR0_EL1
// TTBR1_EL1 always points to swapper_pg_dir (or KPTI kernel stub)
```

---

## 7. Page Table Walk in Hardware

The ARM64 MMU performs the walk automatically on TLB miss:

```
1. CPU issues memory access to VA
2. TLB lookup: (VA, ASID) → miss
3. PTW (Page Table Walker) starts:
   a. Read TTBR0 or TTBR1 based on VA sign bit
   b. Extract PGD index from VA[47:39]
   c. Memory read: PGD[index] (PTW cache may serve this)
   d. Check descriptor type (valid? table? block? page?)
   e. Repeat for each level
4. Final physical address computed
5. Attributes checked (permissions, memory type)
6. TLB filled with new entry
7. Memory access completes
```

Total walk cost: 4 memory accesses (L0, L1, L2, L3) + final access.
With PTW cache: cached level results avoid DRAM accesses.

---

## 8. Interview Questions & Answers

**Q1: How does ARM64 determine which TTBR to use for a given VA?**

By examining the upper bits of the VA. For 48-bit VA (T0SZ=T1SZ=16): if bits[63:48] are all 0x0000, TTBR0_EL1 is used (user space). If bits[63:48] are all 0xFFFF, TTBR1_EL1 is used (kernel space). Any other pattern causes a Translation Fault. The hardware makes this decision before beginning the page table walk, based solely on the upper bits of the VA and the TxSZ values in TCR_EL1.

**Q2: How many memory accesses does a full 4-level page table walk require?**

Four: one read each for L0 (PGD), L1 (PUD), L2 (PMD), and L3 (PTE). Plus the final data access = 5 total memory accesses per TLB miss. The PTW (Page Table Walker) cache can reduce this: if L0/L1/L2 entries haven't changed, only the L3 read may be needed. On a cold TLB, all 4 walk accesses typically hit L2 cache (page tables are frequently accessed, so they are warm in cache).

**Q3: What is the difference between a PTE pointing to a 4KB page vs a PMD pointing to a 2MB block?**

A PTE (L3) descriptor always points to a 4KB physical page. A PMD (L2) can be either a Table Descriptor (pointing to a PTE table) or a Block Descriptor (pointing to a 2MB contiguous physical block). Block descriptors at L2 are "huge pages" — a single TLB entry covers 2MB instead of 4KB, dramatically reducing TLB pressure for large mappings. The descriptor type is indicated by bits[1:0]: `0b01` = Block (at L1/L2), `0b11` = Table (at L0/L1/L2) or Page (at L3).

---

## 9. Quick Reference

| Level | Linux Name | ARM Name | VA Bits | Index Bits | Entry → |
|---|---|---|---|---|---|
| L0 | PGD | L0 | [47:39] | 9 | PUD table PA |
| L1 | PUD | L1 | [38:30] | 9 | PMD table or 1GB block |
| L2 | PMD | L2 | [29:21] | 9 | PTE table or 2MB block |
| L3 | PTE | L3 | [20:12] | 9 | 4KB physical page |
| — | — | Offset | [11:0] | 12 | Within page |

| Item | Size |
|---|---|
| Page | 4 KB |
| PTE table | 4 KB (512 entries × 8B) |
| 2 MB block (PMD) | 512 × 4 KB |
| 1 GB block (PUD) | 512 × 2 MB |
| Full user VA space (one PGD entry) | 512 GB |
