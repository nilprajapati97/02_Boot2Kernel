# Block Descriptors: 1GB (L1), 2MB (L2) — Hardware vs Page-Level Mapping

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. Concept Foundation

ARM64 supports **block descriptors** at L1 (PUD level) and L2 (PMD level) for 4KB granule:
- **L1 block**: Maps 1GB of contiguous physical memory with a single TLB entry
- **L2 block**: Maps 2MB of contiguous physical memory with a single TLB entry

Block descriptors are the foundation of "huge pages" — they skip one or two levels of the page table hierarchy and map directly from a PUD/PMD entry to a large physical block.

Compared to page descriptors:
- **Fewer TLB entries needed**: 1 TLB entry instead of 512 (for 2MB) or 262,144 (for 1GB)
- **Fewer page table levels needed**: PTW stops at L2 or L1 instead of going to L3
- **Natural alignment required**: The physical block must be size-aligned
- **Coarser protection granularity**: All pages in the block share the same attributes

---

## 2. Block Descriptor vs Table vs Page Descriptor

```
bits[1:0] at L1 / L2:
  0b01 = Block Descriptor (maps 1GB at L1, 2MB at L2 for 4KB granule)
  0b11 = Table Descriptor (points to next-level table: L2 or L3)
  0b00 = Invalid

bits[1:0] at L3:
  0b11 = Page Descriptor (maps 4KB)
  0b01 = Invalid (reserved)

Key: Block = 0b01, Page = 0b11 (at L3), Table = 0b11 (at L0/L1/L2)
Hardware knows the level from walk state → same encoding, different meaning
```

---

## 3. L2 Block Descriptor (2MB Huge Page)

### Format

```
Bit 63  59 58  55 54 53 52 51 48 47    21 20   0
 ┌──────┬──────┬──┬──┬──┬─────┬─────────┬──────┐
 │ upper│SWuse │XN│PX│Ct│RES0 │ OA[47:21]│ RES0 │
 │ [63:59│[58:55│54│53│52│51:48│          │[20:0]│
 └──────┴──────┴──┴──┴──┴─────┴─────────┴──────┘
 ┌──┬──┬──┬──┬──┬──┬──────┬──┬──┐
 │nG│AF│SH│AP│NS│  AttrIndx │T │V │
 │11│10│9:8│7:6│5│   [4:2]  │1 │0 │
 └──┴──┴──┴──┴──┴──┴──────┴──┴──┘

V [0]      = 1 (valid)
T [1]      = 0 (Block descriptor, not table)
OA[47:21]  = Output Address bits[47:21]  → PA[47:21], lower 21 bits = 0
             Final PA = OA[47:21] << 21 | VA[20:0]
             VA[20:0] = 21-bit offset within 2MB block

OA alignment: PA must have PA[20:0] = 0 → 2MB-aligned physical memory
```

### Walking to an L2 Block

```
Full walk with L2 block (3 reads only, not 4):
  Step 1: TTBR → PGD read → L0 table descriptor
  Step 2: L0.NextTable → PUD read → L1 table descriptor (→ L2)
  Step 3: L1.NextTable → PMD read → L2 BLOCK descriptor
          → Stop! No L3 read needed
  
  Final PA = PMD_block.OA[47:21] << 21 | VA[20:0]
  
  TLB fill: { VA[47:21], ASID if nG=1 } → { PA[47:21], attrs }
  One TLB entry covers 2MB of VA range
```

### L2 Block TLB Entry

```
Standard 4KB PTE TLB entry:
  VA tag: VA[47:12]    (36 bits of VA tag)
  PA:     PA[47:12]    (36 bits of PA)
  
2MB PMD block TLB entry:
  VA tag: VA[47:21]    (27 bits of VA tag)  ← 9 fewer bits
  PA:     PA[47:21]    (27 bits of PA)
  
The TLB uses VA[47:21] as the tag → 2MB range shares one entry
VA[20:0] is the offset, added to PA[47:21]<<21 to get final PA
```

---

## 4. L1 Block Descriptor (1GB Huge Page)

### Format

```
Bit 63  59 58  55 54 53 52 51 48 47    30 29   0
 ┌──────┬──────┬──┬──┬──┬─────┬─────────┬──────┐
 │ upper│SWuse │XN│PX│Ct│RES0 │ OA[47:30]│ RES0 │
 └──────┴──────┴──┴──┴──┴─────┴─────────┴──────┘

OA[47:30] = bits 47 down to 30 → PA[47:30], lower 30 bits = 0
Final PA = OA[47:30] << 30 | VA[29:0]

PA alignment: PA[29:0] = 0 → 1GB-aligned physical memory
```

### Walking to an L1 Block

```
Full walk with L1 block (only 2 reads!):
  Step 1: TTBR → PGD read → L0 table descriptor
  Step 2: L0.NextTable → PUD read → L1 BLOCK descriptor
          → Stop! No L2 or L3 read needed

Final PA = PUD_block.OA[47:30] << 30 | VA[29:0]

TLB entry covers 1GB of VA range
```

---

## 5. Block Sizes per Granule

```
4KB Granule:
  L2 block: 2 MB    (VA[20:0] = 21-bit offset)
  L1 block: 1 GB    (VA[29:0] = 30-bit offset)
  
16KB Granule:
  L2 block: 32 MB   (16KB × 2048 entries = 32MB)
  L1 block: 64 GB   (32MB × 2048 = 64GB)

64KB Granule:
  L2 block: 512 MB  (64KB × 8192 entries = 512MB)
  L1 block: 512 GB  (512MB × 1024 = 512GB)
```

---

## 6. Linux Huge Page Implementation

### Kernel Linear Map — PMD Blocks

```c
// arch/arm64/mm/mmu.c

static void alloc_init_pmd(pud_t *pud, unsigned long addr,
                            unsigned long end, phys_addr_t phys,
                            pgprot_t prot, ...)
{
    pmd_t *pmdp;
    unsigned long next;
    
    pmdp = pmd_set_fixmap_offset(pud, addr);
    
    do {
        next = pmd_addr_end(addr, end);
        
        // Can we use a 2MB block here?
        if (((addr | next | phys) & ~PMD_MASK) == 0) {
            // addr is PMD_SIZE-aligned
            // next - addr == PMD_SIZE (2MB range)
            // phys is PMD_SIZE-aligned
            // → Use L2 block descriptor
            pmd_t old_pmd = *pmdp;
            write_pmd(pmdp, pfn_pmd(phys >> PAGE_SHIFT, prot));
            
            // Handle old entries if re-mapping:
            if (!pmd_none(old_pmd)) {
                flush_tlb_kernel_range(addr, addr + PMD_SIZE);
            }
        } else {
            // Not 2MB-aligned → use 4KB PTEs
            alloc_init_cont_pte(pmdp, addr, next, phys, prot, ...);
        }
        
        phys += next - addr;
    } while (pmdp++, addr = next, addr != end);
}
```

### User Space Huge Pages (HugeTLB)

```c
// mm/hugetlb.c

// mmap(NULL, 2MB, PROT_RW, MAP_HUGETLB | MAP_ANONYMOUS, -1, 0)
// → Allocates 2MB physically contiguous memory (huge_page_order = 9 for 2MB)
// → Maps using set_huge_pte_at() which installs a PMD block descriptor

void set_huge_pte_at(struct mm_struct *mm, unsigned long addr,
                      pte_t *ptep, pte_t pte)
{
    // For 2MB huge pages, ptep actually points to a pmd_t
    // pte value has OA[47:21] set (2MB block PA)
    set_pmd_at(mm, addr, (pmd_t *)ptep, pmd_mkhuge(*(pmd_t *)&pte));
}

// pmd_mkhuge: ensure bits[1:0] = 0b01 (block, not page/table)
static inline pmd_t pmd_mkhuge(pmd_t pmd)
{
    return __pmd(pmd_val(pmd) & ~PMD_TABLE_BIT);
    // Clear bit[1] → 0b01 (block) instead of 0b11 (table/page)
}
```

### Transparent Huge Pages (THP)

```c
// mm/huge_memory.c: do_huge_pmd_anonymous_page()
// When anonymous page fault is in 2MB-aligned VMA region:

static int do_huge_pmd_anonymous_page(struct vm_fault *vmf)
{
    // Allocate 2MB physically contiguous anonymous page:
    hpage = alloc_hugepage_vma(GFP_TRANSHUGE, vma, vmf->address,
                                HPAGE_PMD_ORDER);
    // HPAGE_PMD_ORDER = 9 for 4KB pages (2^9 = 512 pages = 2MB)
    
    if (!hpage) {
        // Cannot allocate 2MB contiguous → fall back to 4KB pages
        return VM_FAULT_FALLBACK;
    }
    
    // Install PMD block descriptor:
    entry = mk_huge_pmd(hpage, vma->vm_page_prot);
    set_pmd_at(mm, vmf->address & HPAGE_PMD_MASK, vmf->pmd, entry);
    
    // Now the 2MB PMD entry has:
    // OA = physical address of hpage
    // bits[1:0] = 0b01 (block)
    // attributes = prot + AF=1, nG=1
}
```

---

## 7. 1GB Blocks in the Kernel

```c
// 1GB blocks used in kernel linear map for large PA ranges:

static void alloc_init_pud(pgd_t *pgd, unsigned long addr,
                            unsigned long end, phys_addr_t phys, ...)
{
    pud_t *pudp;
    unsigned long next;
    
    pudp = pud_set_fixmap_offset(pgd, addr);
    
    do {
        next = pud_addr_end(addr, end);
        
        // Can we use a 1GB block?
        if (((addr | next | phys) & ~PUD_MASK) == 0) {
            // All 1GB-aligned → use L1 block descriptor
            write_pud(pudp, pfn_pud(phys >> PAGE_SHIFT, prot));
        } else {
            // Not 1GB aligned → must use L2 tables
            alloc_init_cont_pmd(pudp, addr, next, phys, prot, ...);
        }
        
        phys += next - addr;
    } while (pudp++, addr = next, addr != end);
}
```

---

## 8. Splitting Block Descriptors (Huge Page Splitting)

When a process writes to a CoW 2MB huge page, or `mprotect()` changes permissions on part of a huge page, Linux must **split** the block into 4KB PTEs:

```c
// arch/arm64/mm/hugetlbpage.c or mm/huge_memory.c

int split_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
                    unsigned long address)
{
    struct page *page = pmd_page(*pmd);
    pte_t *pte;
    int i;
    
    // Allocate a new PTE-level table (4KB, 512 entries):
    pte = alloc_page(GFP_KERNEL);
    
    // Break-Before-Make:
    // 1. Clear the PMD (set to invalid):
    pmd_clear(pmd);
    // 2. Flush TLB for the 2MB range:
    flush_tlb_range(vma, address & PMD_MASK, (address & PMD_MASK) + PMD_SIZE);
    // DSB + TLBI happens inside flush_tlb_range
    
    // 3. Fill the new PTE table:
    for (i = 0; i < PTRS_PER_PTE; i++) {
        // Each 4KB sub-page within the 2MB block:
        struct page *subpage = page + i;
        pte_t new_pte = mk_pte(subpage, vma->vm_page_prot);
        set_pte_at(vma->vm_mm, address + i * PAGE_SIZE, pte + i, new_pte);
    }
    
    // 4. Install new table descriptor in PMD (Make):
    set_pmd(pmd, __pmd(__pa(pte) | PMD_TABLE_BIT | PMD_TYPE_TABLE));
    // bits[1:0] = 0b11 → Table descriptor
}
```

---

## 9. Interview Questions & Answers

**Q1: How does a 2MB block descriptor differ from a 4KB page descriptor?**

A 2MB block descriptor at L2 (PMD) level has bits[1:0]=0b01 (Block), while a 4KB page descriptor at L3 has bits[1:0]=0b11 (Page). The block descriptor stores OA[47:21] — only the upper 27 bits of the physical address, since the lower 21 bits come from VA[20:0] (the 2MB offset). The page descriptor stores OA[47:12] — upper 36 bits, with VA[11:0] as the offset. The hardware PTW stops at the PMD level for a block — no L3 table read is needed, saving one memory access. One 2MB TLB entry covers the same VA range as 512 4KB TLB entries.

**Q2: What are the constraints on using a 1GB block descriptor?**

The physical address must be 1GB-aligned (PA[29:0] = 0). The virtual address must also be 1GB-aligned. The entire 1GB range must have uniform memory attributes — you can't make part read-only and part writable with a single block entry. 1GB blocks are used in the kernel linear map where large physical memory regions are contiguous and have uniform cache attributes (Normal WB). If any page in the 1GB region needs different attributes (e.g., a device-mapped page), the block must be split into 2MB PMD entries or 4KB PTEs.

**Q3: What happens when a transparent huge page must be split (e.g., due to mprotect on part of it)?**

Linux performs a Break-Before-Make sequence: (1) clears the PMD entry to invalid (Break); (2) flushes the TLB for the 2MB range via `TLBI VAE1IS + DSB`; (3) allocates a new 4KB PTE table; (4) fills the PTE table with 512 individual page descriptors, each with the appropriate attributes for its sub-region; (5) installs a Table Descriptor in the PMD pointing to the new PTE table (Make). After splitting, `mprotect` can change permissions on any individual 4KB page independently. This is why calling `mprotect` on a sub-region of a huge page is more expensive — it forces a huge page split.

---

## 10. Quick Reference

| Level | Granule | Block Size | PA Alignment | TLB Coverage |
|---|---|---|---|---|
| L2 (PMD) | 4 KB | 2 MB | 2 MB | 2 MB per TLB entry |
| L1 (PUD) | 4 KB | 1 GB | 1 GB | 1 GB per TLB entry |
| L2 (PMD) | 16 KB | 32 MB | 32 MB | 32 MB per TLB entry |
| L1 (PUD) | 16 KB | 64 GB | 64 GB | 64 GB per TLB entry |
| L2 | 64 KB | 512 MB | 512 MB | 512 MB per TLB entry |

| Descriptor type | bits[1:0] | Level | Walk depth |
|---|---|---|---|
| Invalid | 00 | Any | Fault |
| Block | 01 | L1/L2 | Stops here |
| Table | 11 | L0/L1/L2 | Continues |
| Page | 11 | L3 | Stops here (leaf) |
