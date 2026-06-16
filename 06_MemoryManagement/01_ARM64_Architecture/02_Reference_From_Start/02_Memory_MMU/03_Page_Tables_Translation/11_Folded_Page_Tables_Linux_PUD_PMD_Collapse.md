# Folded Page Tables: Linux PUD/PMD Collapse

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

Linux supports multiple architectures with different numbers of actual page table levels (2 to 5 levels). Rather than maintaining separate code for each configuration, Linux uses a **generic 5-level page table abstraction** (since kernel 4.11) with the levels:

```
P4D → PGD → P4D → PUD → PMD → PTE → Page
```

When an architecture uses fewer levels than 5, unused levels are "folded" (collapsed) — they exist as types in the code but are implemented as pass-through: the fold-level's entry is the same as the next level's entry. Folding levels is a compile-time mechanism — no runtime overhead.

---

## 2. Linux Page Table Level Names

```
5-level (Linux 4.11+, used for 57-bit VA):
  PGD → P4D → PUD → PMD → PTE → Physical Page

4-level (standard ARM64, 48-bit VA, 4KB granule):
  PGD → (P4D folded = PGD) → PUD → PMD → PTE → Physical Page

3-level (ARM64, 39-bit VA, 4KB granule):
  PGD → (P4D folded = PGD) → (PUD folded = PGD) → PMD → PTE → Physical Page
  
  In Linux terminology for 3-level:
    pgd_t is the only real top-level
    pud_t is folded (pgd_t == pud_t at the implementation level)
    pmd_t is the first real mid-level
    pte_t is the leaf
```

---

## 3. ARM64 Page Table Level Configurations

```
CONFIG_PGTABLE_LEVELS controls how many levels are compiled:

CONFIG_PGTABLE_LEVELS=4 (default for 48-bit VA, 4KB)
  PGDIR_SHIFT = 39 → PGD indexes VA[47:39]
  PUD_SHIFT   = 30 → PUD indexes VA[38:30]
  PMD_SHIFT   = 21 → PMD indexes VA[29:21]
  PAGE_SHIFT  = 12 → PTE indexes VA[20:12]

CONFIG_PGTABLE_LEVELS=3 (for 39-bit VA, 4KB or 42-bit VA)
  PGDIR_SHIFT = 30 → PGD (acts as PUD) indexes VA[38:30]
  PMD_SHIFT   = 21 → PMD indexes VA[29:21]
  PAGE_SHIFT  = 12 → PTE indexes VA[20:12]
  PUD is folded: pud_t == pgd_t

CONFIG_PGTABLE_LEVELS=2 (for 64KB granule, 42-bit VA)
  PGDIR_SHIFT = 29 → PGD (acts as PMD) indexes VA[41:29]
  PAGE_SHIFT  = 16 → PTE indexes VA[28:16]
  Both PUD and PMD are folded

CONFIG_PGTABLE_LEVELS=5 (for 52-bit VA with LPA2)
  P4D level enabled
  P4D_SHIFT = 48 → P4D indexes VA[51:48]
  PGDIR_SHIFT = 39, PUD_SHIFT = 30, PMD_SHIFT = 21, PAGE_SHIFT = 12
```

---

## 4. How Folding Works in Code

### Folded PUD Example (3-level, 4KB)

```c
// When CONFIG_PGTABLE_LEVELS=3:
// arch/arm64/include/asm/pgtable-nopud.h (or include/asm-generic/pgtable-nopud.h)

// pud_t is defined as the same as pgd_t:
typedef struct { pgd_t pgd; } pud_t;

// pud_offset: simply returns a cast pointer to the same pgd entry:
static inline pud_t *pud_offset(pgd_t *pgd, unsigned long addr)
{
    return (pud_t *)pgd;   // No-op cast: PUD IS the PGD for 3-level
}

// pud_page_paddr: same as pgd_page_paddr
static inline phys_addr_t pud_page_paddr(pud_t pud)
{
    return pgd_page_paddr(__pud_pgd(pud));
}

// pud_none, pud_present, etc.: delegate to pgd_ equivalents
#define pud_none(pud)       pgd_none(__pud_pgd(pud))
#define pud_present(pud)    pgd_present(__pud_pgd(pud))
#define pud_clear(pudp)     pgd_clear((pgd_t *)(pudp))
```

### Walking Folded Page Tables

```c
// Generic kernel code (mm/memory.c):
// This code works for ANY number of levels:

pgd_t *pgd = pgd_offset(mm, addr);    // Level 0: always exists
p4d_t *p4d = p4d_offset(pgd, addr);  // Level 1: folded if <5 levels
pud_t *pud = pud_offset(p4d, addr);  // Level 2: folded if <4 levels
pmd_t *pmd = pmd_offset(pud, addr);  // Level 3: folded if <3 levels
pte_t *pte = pte_offset_map(pmd, addr); // Level 4: always leaf

// When PUD is folded (3-level):
//   p4d_offset(pgd, addr) → returns pgd (same pointer, no memory access)
//   pud_offset(p4d, addr) → returns p4d cast as pud (no memory access)
//   pmd_offset(pud, addr) → REAL lookup: reads memory to find PMD entry
//   Total levels: PGD → PMD (two real hops) → PTE (one real hop)

// The compiler optimizes away the folded levels at compile time:
//   pud_offset(p4d, addr) with folded PUD becomes:
//   → return (pud_t *)p4d;  → effectively free
```

---

## 5. Linux ARM64 Build Configurations

```c
// arch/arm64/Kconfig:

config ARM64_VA_BITS
    int
    default 39 if ARM64_VA_BITS_39
    default 48 if ARM64_VA_BITS_48
    default 52 if ARM64_VA_BITS_52

config PGTABLE_LEVELS
    int
    default 2 if ARM64_64K_PAGES && ARM64_VA_BITS_42
    default 3 if ARM64_64K_PAGES && ARM64_VA_BITS_48
    default 3 if ARM64_4K_PAGES  && ARM64_VA_BITS_39
    default 3 if ARM64_16K_PAGES && ARM64_VA_BITS_36
    default 4 if ARM64_4K_PAGES  && ARM64_VA_BITS_48
    default 4 if ARM64_16K_PAGES && ARM64_VA_BITS_48
    default 4 if ARM64_64K_PAGES && ARM64_VA_BITS_52  // LPA2
    default 5 if ARM64_4K_PAGES  && ARM64_VA_BITS_52  // LPA2
```

---

## 6. PMD Collapse (Huge Page Promotion)

"PMD collapse" is different from compile-time folding — it's the runtime process of **promoting** multiple adjacent PTEs into a single PMD block descriptor (2MB huge page):

```c
// arch/arm64/mm/hugetlbpage.c / mm/huge_memory.c

// Transparent Huge Pages (THP) promotion:
// 512 consecutive 4KB PTEs → single 2MB PMD block

// Conditions for collapse:
// 1. All 512 PTEs in a 2MB-aligned range are present and mapped
// 2. All map to consecutive PAs (forming a 2MB PA range)
// 3. All have identical attributes (RW, Normal, etc.)
// 4. The VMA covers the entire 2MB range
// 5. No PTEs are write-protected, locked, or pinned

static int collapse_huge_page(struct mm_struct *mm,
                               unsigned long address, int referenced,
                               int unmapped, struct page *hpage)
{
    // 1. Allocate 2MB contiguous physical memory (order-9 allocation)
    hpage = alloc_contig_range(...);
    
    // 2. Copy content from 512 × 4KB pages to the 2MB page
    
    // 3. Replace the PTE entries with a single PMD block descriptor:
    //    Use Break-Before-Make:
    //    a. zap_pmd_range (invalidate all 512 PTEs + TLB flush)
    //    b. set_pmd_at: install 2MB block descriptor in PMD
    
    // Benefits:
    // - 512 TLB entries → 1 TLB entry (512× TLB pressure reduction)
    // - 512 PTE-level page faults → 0 (entire 2MB handled at once)
    // - Better hardware prefetch (contiguous PA accesses)
}
```

---

## 7. PGD Sharing Between Processes

```c
// Process page tables share kernel PGD entries:
// When a new process is created (fork/exec), its PGD is initialized
// by copying the kernel-half PGD entries from init_mm.pgd:

// arch/arm64/mm/pgd.c
pgd_t *pgd_alloc(struct mm_struct *mm)
{
    pgd_t *new_pgd = (pgd_t *)__get_free_page(GFP_KERNEL);
    
    // Copy upper half (kernel PGDs, for TTBR1 region) from swapper:
    // (For 48-bit VA with folded P4D: upper 256 entries are kernel)
    // In practice TTBR1 is always swapper_pg_dir; TTBR0 always has user PGD.
    // So user PGD only needs the lower half (VA[47:39] < 256) initialized.
    
    memset(new_pgd, 0, PGD_SIZE);  // Clear user entries (invalid)
    // Kernel entries are in TTBR1_EL1 (swapper_pg_dir) — never in TTBR0
    
    return new_pgd;
}
```

---

## 8. Verifying Levels at Runtime

```bash
# Check page table levels from kernel boot log:
dmesg | grep "pgtable"
# Typical output:
# [    0.000000] Page Tables  : folded

# Or check kernel config:
grep PGTABLE_LEVELS /boot/config-$(uname -r)
# CONFIG_PGTABLE_LEVELS=4

# From debugfs (if kernel compiled with CONFIG_PTDUMP):
cat /sys/kernel/debug/kernel_page_tables
# Shows all kernel page table entries and levels
```

---

## 9. Interview Questions & Answers

**Q1: What does "folded page table" mean in Linux, and how does it avoid code duplication?**

Folded page tables allow Linux's generic memory management code to use 4-5 level page table APIs on architectures that implement fewer levels. When a level is "folded," its `_offset()` function returns a pointer to the previous level's entry (a no-op cast), and predicates like `pud_none()` are defined in terms of `pgd_none()`. Compiler optimization eliminates the folded level at compile time — it generates zero instructions for the folded level's functions. This lets all `mm/` code use the generic `pgd → p4d → pud → pmd → pte` walk without `#ifdef` on the level count.

**Q2: How many page table levels does a standard ARM64 Linux kernel use with 4KB pages and 48-bit VA?**

Four levels: `CONFIG_PGTABLE_LEVELS=4`. The hierarchy is PGD (L0, 9 bits, VA[47:39]) → PUD (L1, 9 bits, VA[38:30]) → PMD (L2, 9 bits, VA[29:21]) → PTE (L3, 9 bits, VA[20:12]) → 4KB page (12-bit offset VA[11:0]). Each level table is exactly 4KB (512 entries × 8 bytes). If the kernel is configured for 39-bit VA, only 3 levels are used (`CONFIG_PGTABLE_LEVELS=3`) and PUD is folded into PGD.

**Q3: What is the difference between compile-time page table folding and runtime PMD collapse?**

Compile-time folding reduces the number of active page table levels for a given granule/VA-width configuration. It is purely a code-level abstraction — the hardware uses fewer levels, and the software simply has no-op wrappers for unused levels. Runtime PMD collapse (Transparent Huge Page promotion) takes an existing 4-level mapping at the PTE level and promotes it to a 2MB block at the PMD level, replacing 512 PTE entries with one PMD block descriptor. This is a runtime optimization for application memory with large contiguous access patterns, reducing TLB pressure by 512×.

---

## 10. Quick Reference

| CONFIG_PGTABLE_LEVELS | Granule | VA Bits | Active Levels | Folded Levels |
|---|---|---|---|---|
| 2 | 64 KB | 42 | PGD, PTE | PUD, PMD |
| 3 | 4 KB | 39 | PGD, PMD, PTE | PUD |
| 3 | 16 KB | 36 | PGD, PTE | PUD, PMD |
| 3 | 64 KB | 48 | PGD, PMD, PTE | PUD |
| 4 | 4 KB | 48 | PGD, PUD, PMD, PTE | none |
| 4 | 16 KB | 48 | PGD, PUD, PMD, PTE | none |
| 5 | 4 KB | 52 | P4D, PGD, PUD, PMD, PTE | none |

| Term | Meaning |
|---|---|
| Folded level | Compile-time: level is pass-through, no hardware level |
| PMD collapse | Runtime: 512 PTEs promoted to single 2MB PMD block (THP) |
| PGTABLE_LEVELS | Kernel config: number of active hardware translation levels |
| P4D | Page 4-level Directory: 5th level added for 57-bit VA in x86_64/ARM64 |
