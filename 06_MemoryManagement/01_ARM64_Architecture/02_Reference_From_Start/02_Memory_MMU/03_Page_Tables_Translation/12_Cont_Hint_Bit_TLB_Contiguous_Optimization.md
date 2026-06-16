# Contiguous Hint Bit: TLB Contiguous Optimization

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

The **Contiguous hint** (bit[52] in page/block descriptors) is an ARM64 optimization that allows the TLB to cache a group of consecutive mappings as a single entry. This dramatically reduces TLB entry usage for large, contiguous memory regions.

Without contiguous hint:
- Every 4KB page needs its own TLB entry
- A 64KB region = 16 separate TLB entries
- A 2MB huge page region = 512 TLB entries (without PMD-level block)

With contiguous hint:
- A 64KB-aligned, 64KB-contiguous region with Cont=1 on all 16 PTEs = **1 TLB entry**
- A 2MB PMD block with Cont=1 on 16 consecutive PMDs = **1 TLB entry covering 32MB**

---

## 2. Contiguous Hint Rules

### 4KB Granule — Leaf Level (PTE, L3)

```
Required group: 16 consecutive L3 page descriptors
Covered PA: 16 × 4KB = 64KB
PA alignment: 64KB-aligned (PA[15:0] = 0 for first entry)

Group requirements:
  1. Exactly 16 consecutive entries (indices I to I+15 in same PTE table)
  2. First entry: PA must be 64KB-aligned
  3. All 16 entries: Cont bit = 1
  4. All 16 entries: identical attributes (AttrIndx, AP, SH, PXN, UXN, nG)
  5. Entries map consecutive 4KB physical pages starting from 64KB-aligned PA

TLB result: One "contig TLB entry" covers VA[63:16]→PA[47:16]
            (upper 16-bit of PA fixed; lower 16 bits from VA offset)
```

### 4KB Granule — Block Level (PMD, L2)

```
Required group: 16 consecutive L2 block descriptors
Covered PA: 16 × 2MB = 32MB
PA alignment: 32MB-aligned (PA[24:0] = 0 for first entry)

TLB result: One "contig TLB entry" covers 32MB
```

### 16KB Granule — Leaf Level (L3)

```
Required group: 32 consecutive L3 descriptors (not 16!)
Covered PA: 32 × 16KB = 512KB
PA alignment: 512KB-aligned

Note: The contiguous group size varies by granule:
  4KB leaf: 16 entries (64KB)
  16KB leaf: 32 entries (512KB)
  64KB leaf: 32 entries (2MB)
```

### 64KB Granule — Leaf Level (L2/L3 depending on config)

```
Required group: 32 consecutive descriptors
Covered PA: 32 × 64KB = 2MB
PA alignment: 2MB-aligned
```

---

## 3. Contiguous Hint Group Sizes Table

```
Granule │ Level │ Group Size │ Total Coverage │ PA Alignment
────────┼───────┼────────────┼────────────────┼─────────────
4 KB    │  L3   │ 16 entries │ 64 KB          │ 64 KB
4 KB    │  L2   │ 16 entries │ 32 MB          │ 32 MB
4 KB    │  L1   │ 16 entries │ 16 GB          │ 16 GB
16 KB   │  L3   │ 32 entries │ 512 KB         │ 512 KB
16 KB   │  L2   │ 32 entries │ 1 GB           │ 1 GB
64 KB   │  L2   │ 32 entries │ 2 MB (leaf)    │ 2 MB
64 KB   │  L1   │ 32 entries │ 16 GB          │ 16 GB
```

---

## 4. Implementation Constraint: Atomic Group Operations

**Critical rule**: When using contiguous hint, all entries in a group must be **modified together**. Modifying a single entry while others still have Cont=1 is UNPREDICTABLE.

### Correct Update Sequence

```c
// To change attributes of a contiguous group:
// 1. BREAK: Invalidate all 16 entries (set to 0 = invalid)
for (i = 0; i < CONT_PTES; i++) {
    pte_clear(mm, addr + i * PAGE_SIZE, ptep + i);
}
// 2. DSB + TLBI flush for the entire 64KB range
flush_tlb_range(vma, addr, addr + CONT_SIZE);

// 3. MAKE: Set new descriptors with updated attributes
for (i = 0; i < CONT_PTES; i++) {
    set_pte_at(mm, addr + i * PAGE_SIZE, ptep + i,
               pte_mkcont(new_pte_for_page[i]));
}
// pte_mkcont() sets bit[52] = 1
```

### Incorrect (DO NOT DO):

```c
// WRONG: Modifying one entry in a contiguous group
set_pte_at(mm, addr, ptep, pte_mkrdonly(*ptep));
// ^ UNPREDICTABLE: other 15 entries still have Cont=1
// The TLB may have cached a stale 64KB contig entry that now covers
// a mix of old and new attributes → security and correctness issue
```

---

## 5. Linux Kernel Implementation

```c
// arch/arm64/include/asm/pgtable.h

// CONT_PTES: number of PTEs in a contiguous group
#ifdef CONFIG_ARM64_4K_PAGES
#define CONT_PTES           16
#define CONT_SIZE           (CONT_PTES * PAGE_SIZE)    // 64 KB
#define CONT_MASK           (~(CONT_SIZE - 1))         // 64KB alignment mask

// For PMD level (4KB granule, L2 blocks):
#define CONT_PMDS           16
#define CONT_PMD_SIZE       (CONT_PMDS * PMD_SIZE)     // 32 MB
#define CONT_PMD_MASK       (~(CONT_PMD_SIZE - 1))
#endif

// Check/set contiguous bit:
static inline pte_t pte_mkcont(pte_t pte)
{
    return set_pte_bit(pte, __pgprot(PTE_CONT));  // PTE_CONT = 1UL << 52
}

static inline pte_t pte_mknoncont(pte_t pte)
{
    return clear_pte_bit(pte, __pgprot(PTE_CONT));
}

static inline bool pte_cont(pte_t pte)
{
    return !!(pte_val(pte) & PTE_CONT);
}
```

### Kernel Linear Map with Contiguous Hint

```c
// arch/arm64/mm/mmu.c
// When creating the kernel linear map, Linux uses contiguous hints
// for 64KB-aligned regions:

static void alloc_init_cont_pte(pmd_t *pmd, unsigned long addr,
                                 unsigned long end, phys_addr_t phys, ...)
{
    unsigned long next;
    pte_t *ptep;
    
    // Check if we can use contiguous hint:
    // - addr is 64KB-aligned
    // - end - addr >= 64KB
    // - phys is 64KB-aligned
    
    do {
        next = min(ALIGN(addr + 1, CONT_SIZE), end);
        
        if ((addr | next | phys) & ~CONT_MASK) {
            // Not aligned for contiguous: map one page at a time
            alloc_init_pte(..., false);  // Cont=0
        } else {
            // Use contiguous hint: map 16 pages at once
            // addr..next covers exactly one contiguous group
            alloc_init_pte(..., true);   // Cont=1 for all 16 PTEs
        }
        phys += next - addr;
    } while (addr = next, addr != end);
}
```

---

## 6. Contiguous Hint and Huge Pages

```
Contiguous hint is NOT the same as huge pages (block mappings):

Huge page (2MB, PMD block):
  - Single L2 block descriptor (bits[1:0] = 0b01)
  - maps 2MB with ONE TLB entry
  - Mapped in PMD table (no PTE level needed)
  
Contiguous 64KB (16 PTEs with Cont=1):
  - 16 L3 page descriptors, all with bit[52]=1
  - Maps 64KB with ONE TLB entry
  - Still uses PTE table (full 4-level walk)
  - More flexible: can have per-page fine-grained attributes
    (same attribute requirement holds within group, but different groups can differ)

Use cases for contiguous hint vs huge pages:
  - 2MB aligned region with 2MB PA → use PMD block (only 1 walk level)
  - 64KB aligned region, not 2MB aligned → use contiguous PTEs
  - Kernel linear map: both contiguous PTEs AND PMD blocks used together
```

---

## 7. Performance Impact

### TLB Entry Savings

```
Map 64MB of kernel linear memory:

Without contiguous hint:
  64MB / 4KB = 16,384 TLB entries needed

With contiguous hint (64KB groups):
  64MB / 64KB = 1,024 TLB entries needed
  Savings: 93.75%

With PMD blocks (2MB):
  64MB / 2MB = 32 TLB entries needed
  Savings: 99.8%

With contiguous PMD blocks (32MB groups):
  64MB / 32MB = 2 TLB entries needed
  Savings: 99.99%
```

### Linux Real-World Benefit

```
Kernel linear map on a 16GB system:
  Without optimizations: 16GB / 4KB = 4,194,304 TLB entries (impossible — TLB has ~2048)
  
  With PMD blocks (2MB): 16GB / 2MB = 8,192 entries → still too many for TLB
  
  With contiguous PMD blocks (32MB): 16GB / 32MB = 512 entries → fits in L2 TLB!
  → Entire 16GB linear map can be cached with 512 TLB entries
  → Nearly zero TLB misses for kernel linear memory accesses
  → Critical for memory-intensive workloads (databases, in-memory caches)
```

---

## 8. Interview Questions & Answers

**Q1: What is the contiguous hint bit and how does it reduce TLB pressure?**

Bit[52] in an ARM64 descriptor is the Contiguous hint. When set on a group of consecutive descriptors (16 for 4KB pages, forming 64KB total), the TLB can cache all 16 as a single "contiguous TLB entry" — one TLB entry serving 64KB instead of 4KB. This reduces TLB entries by 16× for 64KB-aligned contiguous mappings. All entries in the group must be set simultaneously, have the same attributes, and map a 64KB-aligned PA range. The kernel uses this extensively for the linear map to fit it into the L2 TLB.

**Q2: What are the strict requirements for setting the Contiguous hint bit?**

Three requirements must all be satisfied: (1) The entries must be exactly N consecutive descriptors within the same page table (N=16 for 4KB-granule L3, N=32 for 16KB/64KB), (2) the first entry must map a PA address aligned to N×page_size (e.g., 64KB-aligned for 16 × 4KB), and (3) all N entries must have identical attributes (AttrIndx, AP, SH, PXN, UXN, nG). Violating any requirement causes UNPREDICTABLE behavior. Additionally, when updating any attribute of a contiguous group, all N entries must be atomically invalidated first (Break-Before-Make for the entire group), then re-installed together.

**Q3: How is the contiguous hint different from a 2MB huge page (PMD block descriptor)?**

A 2MB PMD block (`bits[1:0]=0b01` at L2) eliminates the PTE level entirely — there is no L3 table; the PMD directly maps 2MB with one TLB entry. A contiguous group of 16 × 4KB PTEs still requires a full 4-level walk (PGD→PUD→PMD→PTE), but the TLB can cache all 16 results as one entry. The key difference: a block descriptor requires 2MB-aligned physical memory and removes one level of indirection, while contiguous hint only requires 64KB alignment and keeps all levels intact. Contiguous hint is more flexible (can have per-64KB-group attribute boundaries) but has slightly more walk overhead than a block.

---

## 9. Quick Reference

| Granule | Level | Group Size | Coverage | PA Alignment |
|---|---|---|---|---|
| 4 KB | L3 (PTE) | 16 | 64 KB | 64 KB |
| 4 KB | L2 (PMD block) | 16 | 32 MB | 32 MB |
| 16 KB | L3 | 32 | 512 KB | 512 KB |
| 64 KB | L2/L3 | 32 | 2 MB | 2 MB |

| Linux Constant | Value (4KB) | Meaning |
|---|---|---|
| `PTE_CONT` | `1UL << 52` | Contiguous bit in descriptor |
| `CONT_PTES` | 16 | Number of PTEs per contiguous group |
| `CONT_SIZE` | 64 KB | Coverage per group |
| `CONT_MASK` | `~0xFFFF` | Alignment mask |
| `CONT_PMDS` | 16 | Number of PMDs per contiguous group |
| `CONT_PMD_SIZE` | 32 MB | PMD-level contiguous coverage |
