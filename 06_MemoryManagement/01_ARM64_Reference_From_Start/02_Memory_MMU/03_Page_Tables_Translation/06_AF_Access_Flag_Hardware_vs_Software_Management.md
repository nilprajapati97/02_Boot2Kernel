# AF: Access Flag — Hardware vs Software Management

**Category**: Page Tables and Translation  
**Targeted**: ARM, Qualcomm

---

## 1. Concept Foundation

The **Access Flag (AF)** in an ARM64 page/block descriptor serves as the hardware's mechanism for tracking page access. At bit[10] of the descriptor, it answers a fundamental question for the OS: **"Has this page been accessed since we last checked?"**

This is critical for the page reclaim system:
- Recently accessed pages = "hot" → keep in RAM
- Long-unaccessed pages = "cold" → candidate for reclaim/swap

The AF mechanism lets Linux implement the active/inactive LRU lists and the working set detection algorithm efficiently.

---

## 2. AF Bit States

```
AF=0: Page NOT yet accessed (or marked as "cold" by OS)
  → On ANY access (read or write), hardware generates an Access Flag Fault
  → Fault is handled by the OS, which sets AF=1 and marks the page as accessed

AF=1: Page has been accessed
  → No fault on access; hardware proceeds normally
  → The page is considered "warm"
```

---

## 3. Software AF Management (Before ARMv8.1)

Without hardware AF support, the kernel manually manages AF:

### Step 1: New Page Mapping — AF=0

When a new page is mapped (e.g., on demand paging page fault):

```c
// mm/memory.c: do_fault() → finish_fault() → set_pte_at()
// arch/arm64/include/asm/pgtable.h

// New PTE has AF=0 (cleared) to enable first-access tracking:
pte_t pte = mk_pte(page, vma->vm_page_prot);
// mk_pte does NOT set PTE_AF by default
// → AF=0 in the new mapping
```

### Step 2: First Access — AF Fault

When user/kernel accesses VA with AF=0:

```
Hardware detects AF=0 during translation:
  → Access Flag fault generated (Data Abort)
  → ESR_EL1:
      EC = 0x21 (Data Abort from EL0) or 0x25 (from EL1)
      DFSC[5:0] = 0b00_1000 to 0b00_1011 (AF fault at L0-L3)
        DFSC = 0x08 = AF fault at L0
        DFSC = 0x09 = AF fault at L1
        DFSC = 0x0A = AF fault at L2
        DFSC = 0x0B = AF fault at L3 (most common for 4KB pages)
  → FAR_EL1 = faulting VA
```

### Step 3: AF Fault Handler

```c
// arch/arm64/mm/fault.c
static int handle_pte_fault(struct vm_fault *vmf)
{
    pte_t entry = vmf->orig_pte;
    
    if (!pte_present(entry)) {
        // Page not present — different handling
    }
    
    // Check if this is just an AF fault:
    if (pte_protnone(entry) && vma_is_accessible(vmf->vma)) {
        return do_numa_page(vmf);  // NUMA balancing case
    }
    
    // Set AF=1 and mark page accessed:
    if (flags & FAULT_FLAG_WRITE) {
        entry = pte_mkdirty(entry);
    }
    entry = pte_mkyoung(entry);  // Sets AF=1 + marks struct page accessed
    
    set_pte_at(vmf->vma->vm_mm, vmf->address, vmf->pte, entry);
    // Now AF=1 → no more faults on this page until aged again
}

// pte_mkyoung:
static inline pte_t pte_mkyoung(pte_t pte)
{
    return set_pte_bit(pte, __pgprot(PTE_AF));
}
```

### Step 4: Page Aging (Clearing AF=0 for Reclaim)

The kernel periodically ages pages by clearing AF:

```c
// mm/vmscan.c: shrink_page_list() calls page_referenced()
// mm/rmap.c: page_referenced_one() → ptep_clear_flush_young()

// arch/arm64/include/asm/pgtable.h:
static inline int ptep_test_and_clear_young(struct vm_area_struct *vma,
                                             unsigned long addr, pte_t *ptep)
{
    pte_t old_pte, pte;
    
    pte = READ_ONCE(*ptep);
    if (!pte_young(pte))
        return 0;
    
    // Atomically clear AF bit:
    old_pte = ptep_modify_prot_start(vma->vm_mm, addr, ptep);
    pte = pte_mkold(old_pte);  // Clear PTE_AF
    ptep_modify_prot_commit(vma->vm_mm, addr, ptep, old_pte, pte);
    
    return 1;  // page WAS young (AF=1), now aged
}
```

---

## 4. Hardware AF Management (ARMv8.1 — TCR_EL1.HA)

ARMv8.1 introduces hardware AF management, eliminating the need for software AF faults:

### Enabling Hardware AF

```c
// arch/arm64/mm/proc.S or arch/arm64/include/asm/pgtable.h
// Check if CPU supports HA (hardware Access Flag):
// ID_AA64MMFR1_EL1.HAFDBS[3:0] bits:
//   0b0000 = No hardware AF/DB support
//   0b0001 = Hardware AF supported (TCR.HA)
//   0b0010 = Hardware AF + Hardware Dirty Bit supported (TCR.HA + TCR.HD)

// If supported, Linux sets TCR_EL1.HA = 1:
// arch/arm64/kernel/head.S (cpu_do_resume or create_kpti_ng_temp_pgd):
tcr |= TCR_HA | TCR_HD;  // Enable HW AF + HW dirty bit management
```

### Hardware AF Behavior with HA=1

```
With TCR_EL1.HA=1:
  When hardware walks page tables and finds a valid descriptor with AF=0:
    → Hardware atomically sets AF=1 in the page table entry
    → No fault generated
    → Access proceeds normally (transparent to software)
    
  Hardware uses "hardware update" mechanism:
    → Requires write permission to the page table itself (table memory must be writable)
    → The AF update is atomic from the perspective of other PTW or DSB
```

### Performance Impact

```
Without HA (software AF):
  Every first access → fault → exception handler → context save/restore → set AF → return
  Cost: ~500-1000 cycles per page first access

With HA (hardware AF):
  First access → hardware sets AF in place → access continues (no exception)
  Cost: 1-3 extra cycles for the hardware page table write

For a workload mapping 1 million pages (typical):
  Software AF: 1M × 500 cycles = 500M cycles wasted
  Hardware AF: ~3M cycles (negligible)
→ Hardware AF can reduce page fault overhead by 99%+ for workloads with dense page access
```

---

## 5. AF Fault at Different Translation Levels

```
DFSC encoding for AF faults:
  0b00_1000 (0x08) = AF fault, Level 0
  0b00_1001 (0x09) = AF fault, Level 1  ← L1 block with AF=0
  0b00_1010 (0x0A) = AF fault, Level 2  ← 2MB block with AF=0
  0b00_1011 (0x0B) = AF fault, Level 3  ← 4KB page with AF=0 (most common)

For a 2MB huge page with AF=0:
  → AF fault with DFSC=0x0A
  → Kernel sets AF=1 in the PMD (block descriptor)
  → One fault covers the entire 2MB block (efficient)

For a 1GB block with AF=0:
  → AF fault with DFSC=0x09
  → One fault covers the entire 1GB region
```

---

## 6. AF in the LRU Page Reclaim Flow

```
Linux page reclaim (mm/vmscan.c):

1. kswapd or memory pressure triggers shrink_lruvec()
2. isolate_lru_pages() picks candidate pages (from inactive list)
3. page_referenced() checks if page was recently accessed:
   → Calls ptep_clear_flush_young() for each mapping of the page
   → If AF=1 was found (page was accessed): page is "referenced"
       → Move page back to active list (keep in RAM)
   → If AF=0 (page not accessed since last check): page is cold
       → Page stays in inactive list, candidate for reclaim

4. On second pass (still cold): page is swapped out or freed

The AF bit is cleared in step 3 regardless:
  → Next pass will check freshness from that point on
  → Implements an approximation of LRU order using the AF bit
```

---

## 7. AF and NUMA Balancing

ARM64 Linux uses AF=0 for NUMA page migration hints:

```c
// mm/mprotect.c: change_pte_range() for NUMA balancing
// Set AF=0 (called pte_protnone in code) to trigger faults:

if (do_numa_page) {
    pte = pte_mkold(pte);   // Clear AF → trigger access fault
    pte = pte_mknuma(pte);  // Mark as NUMA migration candidate
    // Map as PROT_NONE temporarily (causes fault on any access)
}

// On fault:
// do_numa_page() checks if the page is on the "right" NUMA node
// (the node closest to the CPU that faulted)
// If wrong node → migrate page to closer node
// This is "NUMA page migration" or "memory tiering"
```

---

## 8. Interview Questions & Answers

**Q1: What happens when an ARM64 CPU accesses a page with AF=0?**

The hardware generates an Access Flag Fault. The exception syndrome in `ESR_EL1` has `EC=0x21` (data abort from EL0) or `EC=0x25` (from EL1), with `DFSC=0x0B` (AF fault at Level 3 for a 4KB page). The fault handler in `arch/arm64/mm/fault.c` sets `AF=1` in the PTE using `pte_mkyoung()` and marks the corresponding `struct page` as accessed (for LRU accounting). The faulting instruction is then re-executed transparently. This mechanism enables the OS to track which pages are actively used without needing additional data structures.

**Q2: How does ARMv8.1 hardware AF (TCR_EL1.HA=1) eliminate access flag faults?**

With `TCR_EL1.HA=1`, when the hardware page table walker encounters a valid descriptor with `AF=0`, it atomically writes `AF=1` into the descriptor in the page table (the page table memory must be writable). The access then proceeds as if `AF=1` was always set. No exception is generated. This eliminates the thousands of AF faults that occur during typical workload execution, saving 500-1000 cycles per page first access. Linux enables HA when `ID_AA64MMFR1_EL1.HAFDBS >= 1`.

**Q3: How does clearing AF=0 implement page aging for reclaim?**

Linux's LRU reclaim periodically calls `ptep_clear_flush_young()` to atomically read and clear the AF bit. If AF=1 was found (page was accessed since last check), the page is "young" — recently used. If AF=0 was found, the page is "old" — not accessed. The `ptep_test_and_clear_young()` function returns 1 (young) or 0 (old). Young pages are moved back to the active LRU list; old pages remain in the inactive list. On a second scan cycle, inactive, still-old pages are selected for reclaim/swap. This two-chance algorithm (checking AF across multiple scan cycles) provides an approximation of LRU without requiring exact timestamps.

---

## 9. Quick Reference

| Aspect | AF=0 | AF=1 |
|---|---|---|
| Meaning | Not accessed | Accessed |
| On access | AF Fault generated | Normal access |
| DFSC (L3 page) | 0x0B | N/A |
| SW sets to 0 | `pte_mkold()` | — |
| SW sets to 1 | — | `pte_mkyoung()` |
| HW sets (HA=1) | — | Automatically on access |
| LRU effect | Page is "cold" | Page is "hot" |

| ARMv8.x | AF Management |
|---|---|
| Before ARMv8.1 | Software-only (AF faults) |
| ARMv8.1+ | Hardware AF: `TCR_EL1.HA=1` eliminates faults |
| ARMv8.1+ | Hardware Dirty: `TCR_EL1.HD=1` for dirty tracking |
| Linux check | `ID_AA64MMFR1_EL1.HAFDBS` bits |
