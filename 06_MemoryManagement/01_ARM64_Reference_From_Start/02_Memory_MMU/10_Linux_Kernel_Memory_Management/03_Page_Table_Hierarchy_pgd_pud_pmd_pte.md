# Page Table Hierarchy: pgd_t, pud_t, pmd_t, pte_t

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. ARM64 Page Table Hierarchy Overview

```
ARM64 4-level page table (4KB pages, 48-bit VA):

VA bits [47:0]:
  [47:39] = PGD index (9 bits, 512 entries)
  [38:30] = PUD index (9 bits, 512 entries)
  [29:21] = PMD index (9 bits, 512 entries)
  [20:12] = PTE index (9 bits, 512 entries)
  [11:0]  = Page offset (12 bits, 4096 bytes)

Each level:
  512 entries × 8 bytes = 4096 bytes = ONE PAGE per table

Physical address: from PTE bits [47:12] + VA[11:0]
(or PMD[47:21] for 2MB blocks, PUD[47:30] for 1GB blocks)

Linux types:
  pgd_t: PGD entry (L0 on ARM64 with FEAT_LPA2)
  p4d_t: P4D entry (folded = same as PGD for 4-level, separate for 5-level)
  pud_t: PUD entry
  pmd_t: PMD entry
  pte_t: PTE entry

Typedef definitions (arch/arm64/include/asm/pgtable-types.h):
  typedef struct { pteval_t pgd; } pgd_t;
  typedef struct { pteval_t pud; } pud_t;
  typedef struct { pteval_t pmd; } pmd_t;
  typedef struct { pteval_t pte; } pte_t;
  typedef unsigned long pteval_t;  // 64-bit

Using structs (not just typedef of u64):
  Advantage: type safety — pgd_t and pte_t are different types
  Prevents accidentally mixing table levels
  Access the value: pgd_val(pgd) = pgd.pgd; pte_val(pte) = pte.pte
```

---

## 2. Page Table Walking in Linux

```c
/* Walking the page tables for a given virtual address */

// Starting point: mm->pgd (process page table)
//                 init_mm.pgd / swapper_pg_dir (kernel)

pgd_t *pgdp = pgd_offset(mm, vaddr);
// pgd_offset(mm, addr) = mm->pgd + pgd_index(addr)
// pgd_index(addr) = (addr >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1)
// PGDIR_SHIFT = 39 (for 4-level, 4KB pages)
// PTRS_PER_PGD = 512

if (pgd_none(*pgdp) || pgd_bad(*pgdp)) goto error;

p4d_t *p4dp = p4d_offset(pgdp, vaddr);
// On ARM64 without 5-level: p4d_offset = pgd_offset (folded)

pud_t *pudp = pud_offset(p4dp, vaddr);
// pud_index(addr) = (addr >> PUD_SHIFT) & (PTRS_PER_PUD - 1)
// PUD_SHIFT = 30, PTRS_PER_PUD = 512

if (pud_none(*pudp) || pud_bad(*pudp)) goto error;
if (pud_huge(*pudp)) { /* 1GB block found */ handle_huge(); goto done; }

pmd_t *pmdp = pmd_offset(pudp, vaddr);
// pmd_index(addr) = (addr >> PMD_SHIFT) & (PTRS_PER_PMD - 1)
// PMD_SHIFT = 21, PTRS_PER_PMD = 512

if (pmd_none(*pmdp)) goto error;
if (pmd_huge(*pmdp)) { /* 2MB block found */ handle_huge(); goto done; }

pte_t *ptep = pte_offset_kernel(pmdp, vaddr);
// pte_index(addr) = (addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1)
// PAGE_SHIFT = 12, PTRS_PER_PTE = 512

if (pte_none(*ptep)) goto error;
if (!pte_present(*ptep)) { /* page swapped out */ handle_swap(); goto done; }

// Extract physical page number:
unsigned long pfn = pte_pfn(*ptep);  // pte_val(*ptep) >> PAGE_SHIFT (ignoring attr bits)
unsigned long phys = (pfn << PAGE_SHIFT) | (vaddr & ~PAGE_MASK);
struct page *page = pfn_to_page(pfn);
```

---

## 3. PTE Entry Bit Layout (ARM64)

```
64-bit PTE (Block/Page descriptor):

Bits [63:56]: Upper attributes
  [63]    : IGNORE (software bit, Linux uses for dirty tracking)
  [62]    : PXN (Privileged Execute Never) — ARM64 kernel has XN=0 for code
  [61]    : UXN (Unprivileged Execute Never) — set for data pages
  [60]    : CONTIGUOUS (16 contiguous PTEs = performance hint)
  [59:56] : PBHA (Page-Based Hardware Attributes) / MTE tags
  
Bits [55:52]: Software bits (Linux uses these!)
  [55]    : soft-dirty bit (for CRIU memory tracking)
  [54]    : PTE_DEVMAP (mapped as device memory via devm)
  [53]    : PTE_SPECIAL (special mapping, refcounting exception)
  [52]    : PTE_PROT_NONE (memory protected but VMA exists — for mprotect)

Bits [51:48]: PA extension bits (if FEAT_LPA2: bits [51:50] extend PA)
Bits [47:12]: Output address bits [47:12] (physical page frame number)

Bits [11:2]: Lower attributes
  [11]    : nG (not Global — 0=global kernel, 1=ASID-tagged user)
  [10]    : AF (Access Flag — set on first access, cleared by SW for aging)
  [9:8]   : SH (Shareability: 0b00=NS, 0b10=OS, 0b11=IS)
  [7:6]   : AP[2:1] (Access Permissions):
              AP=0b00: EL1 RW, EL0 no access
              AP=0b01: EL1 RW, EL0 RW
              AP=0b10: EL1 RO, EL0 no access
              AP=0b11: EL1 RO, EL0 RO
  [5:2]   : AttrIndx[2:0] (3 bits index into MAIR_EL1) + 1 reserved
              AttrIndx[2:0] = 0b000 to 0b111 → 8 MAIR slots

Bits [1:0]: Entry type
  0b11: page/block descriptor (valid)
  0b01: table descriptor (next level)
  0b00: invalid (pte_none → page not present)

Linux macro helpers:
  pte_present(pte):   checks bit[0] (valid bit)
  pte_write(pte):     checks AP[2] (bit[7]) == 0 (not read-only)
  pte_exec(pte):      checks UXN (bit[54]) == 0
  pte_young(pte):     checks AF bit[10] == 1 (accessed recently)
  pte_dirty(pte):     checks DBM (HW) or soft-dirty bit[55] (SW)
  pte_huge(pte):      checks if PMD or PUD descriptor type
```

---

## 4. Kernel Page Tables

```
ARM64 kernel page tables:

swapper_pg_dir: kernel's PGD (init_mm.pgd)
  Maps: kernel virtual address space (TTBR1_EL1)
  Range: 0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF (48-bit)
  Size: 4KB (512 entries × 8 bytes, 1 entry covers 512GB)
  Allocated: in kernel BSS, statically (arch/arm64/kernel/vmlinux.lds.S)

idmap_pg_dir: identity map for MMU enable/disable
  Maps: kernel PA = VA (1:1)
  Used during: CPU boot, hibernate resume
  Contains: only the kernel code needed to enable the MMU + first jump

tramp_pg_dir: KPTI trampoline (if KPTI enabled)
  Maps: minimal kernel text to handle EL0→EL1 exceptions
  Used: TTBR1 at EL0 exit (Meltdown mitigation, NOT needed for most ARM64)

init_top_pgt (x86 name): equivalent is swapper_pg_dir on ARM64

Per-CPU page tables (not standard in Linux):
  Some architectures: per-CPU page tables for kernel (not ARM64 standard)
  ARM64: swapper_pg_dir is SHARED (same for all CPUs)
  TTBR1_EL1: same on all CPUs (kernel maps are identical)
  TTBR0_EL1: per-CPU (different per task via context switch)

Linux kernel virtual address layout (48-bit):
  0xFFFF_0000_0000_0000: start of kernel VA space
  0xFFFF_0000_FFFF_FFFF: linear map (physmem mapped here)
  0xFFFF_8000_0000_0000: vmalloc area
  0xFFFF_B000_0000_0000: PCI I/O space
  0xFFFF_C000_0000_0000: fixed mappings (fixmap)
  0xFFFF_E000_0000_0000: vmemmap (struct page array)
  0xFFFF_FFFF_A000_0000: kernel image (.text, .data, .bss)
```

---

## 5. Page Table Allocation and Freeing

```
Page table page allocation:

pgtable_alloc = __get_free_page(GFP_PGTABLE_USER)
  Allocates a 4KB page from the buddy allocator
  Uses GFP_PGTABLE_USER flags:
    GFP_PGTABLE_USER = GFP_KERNEL | __GFP_ZERO | __GFP_ACCOUNT
  
  __GFP_ZERO: all entries initialized to 0 (invalid)
  __GFP_ACCOUNT: counts against process's memory limit

PTE page caching (SLUB cache):
  PTE pages managed by pte_cache (pgtable_cache)
  Quicklists per CPU for fast alloc/free
  ARM64: pmd_populate_kernel() / pte_alloc_kernel()

Page table tear-down (munmap/exit):
  free_pgtables() → free_pgd_range() → free_pud_range() → free_pmd_range() → free_pte_range()
  
  For each level: unmap pages, TLB flush, free the page table page
  
  TLB flush MUST precede freeing:
    Without TLB flush: another CPU may use the freed (now-reused) PTE page
    → Use the "wrong" PTE, accessing arbitrary memory!
    
  TLB flush order:
    1. Clear PTE (pte_clear)
    2. TLB flush (flush_tlb_page or flush_tlb_mm)
    3. Free physical page (page_remove_rmap + free_page)
    4. Free the PTE table page (pte_free)

Linux lazy TLB:
  Kernel threads: use init_mm (no user mappings)
  When switching to kernel thread: don't flush TLB (lazy switch)
  activate_mm() / switch_mm() distinction:
    switch_mm(): if next task is kernel thread → enter_lazy_tlb()
                 keep old ASID/TTBR0 (no flush needed)
```

---

## 6. Interview Questions & Answers

**Q1: When does Linux allocate page table pages for a process? Are they allocated eagerly at mmap() time or lazily on page fault?**

Page table pages are allocated **lazily** in Linux — only when actually needed during page fault handling.

When a process calls `mmap()`, Linux creates a **VMA (vm_area_struct)** describing the virtual address range, but NO page table pages are allocated. The PTEs don't exist yet.

When the process actually accesses a virtual address in that range, a **TLB miss** occurs → the hardware page table walker tries to translate the address → finds an invalid PTE or no PTE page at all → generates a **page fault** (Translation fault on ARM64).

The kernel's page fault handler (`do_page_fault()` → `handle_mm_fault()`) walks the page table:
1. If the PGD entry is invalid → allocate a PUD table page (`pud_alloc()`)
2. If the PUD entry is invalid → allocate a PMD table page (`pmd_alloc()`)
3. If the PMD entry is invalid → allocate a PTE table page (`pte_alloc_map()`)
4. Install the PTE with the appropriate physical page

This lazy allocation is critical for efficiency: a process may `mmap()` a large file but only access a small portion — allocating all page table pages upfront would waste memory for the unused mappings.

The exception: `mlock()` / `MAP_LOCKED` forces immediate page faulting (via `populate_vma_page_range()`), which causes all page table pages to be allocated and all physical pages to be faulted in.

---

## 7. Quick Reference

| Level | Shift | Index Bits | Coverage per Entry | ARM64 Name |
|---|---|---|---|---|
| PGD | 39 | bits[47:39] | 512 GB | L1 table |
| PUD | 30 | bits[38:30] | 1 GB | L2 table |
| PMD | 21 | bits[29:21] | 2 MB | L3 table |
| PTE | 12 | bits[20:12] | 4 KB | L3 page |

| PTE Bit | Name | Purpose |
|---|---|---|
| [0] | VALID | Entry is valid (mapped) |
| [6] | AP[1] | EL0 accessible if 1 |
| [7] | AP[2] | Read-only if 1 |
| [10] | AF | Access flag |
| [53] | PXN | Kernel execute never |
| [54] | UXN | User execute never |
| [55] | SOFT-DIRTY | Software dirty tracking |
