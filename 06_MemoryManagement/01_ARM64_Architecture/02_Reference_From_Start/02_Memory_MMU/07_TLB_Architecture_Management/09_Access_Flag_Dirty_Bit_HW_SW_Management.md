# Access Flag (AF) and Dirty Bit Management: Hardware vs Software

**Category**: TLB Architecture & Management  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Access Flag (AF) Overview

```
AF (Access Flag): PTE bit[10]
  Set by hardware (with HA=1 in TCR) OR software (HA=0, default pre-ARMv8.1)
  
  AF=0: This page has NOT been accessed since AF was last cleared
  AF=1: This page HAS been accessed (read or write)
  
  Purpose:
    Page reclaim (kswapd, OOM killer):
    Linux needs to know which pages are "hot" (recently used) vs "cold" (unused)
    Cold pages → good candidates for swapping out or freeing
    Hot pages → keep in memory
    
    AF bit provides the "accessed" signal for the page replacement algorithm
    (Second chance / LRU approximation for page eviction)

AF fault behavior (HA=0, software-managed):
  When CPU accesses a page with AF=0:
    → Access Flag Fault (special fault type)
    → Exception to EL1
    → Linux fault handler sets AF=1 in the PTE
    → TLBI to update TLB entry
    → Resumes the access
  
  Cost: 500–1000 cycles per first access to a new page
  (Exception, handler, PTE update, TLBI, resume)
  
HA (Hardware Access Flag, ARMv8.1):
  TCR_EL1.HA = 1
  Hardware sets AF=1 automatically on first access — NO fault needed
  Cost: ~3–5 cycles (hardware PTE update during TLB fill)
  Benefit: 100–200× faster than software AF management
  Linux uses HA when available: CONFIG_ARM64_HW_AFDBM
```

---

## 2. Dirty Bit Management (DBM)

```
Dirty bit: PTE AP[2] = 1 means read-only, AP[2] = 0 means writable
  Software dirty tracking: CPU must take a fault on first write to clean page
  
Without DBM (software):
  Mark all pages read-only initially (AP[2]=1) after last write-back
  First write: Permission Fault (write to RO page)
  → Linux fault handler: 
    - Marks page dirty (PG_dirty flag in struct page)
    - Changes AP[2]=0 (make writable)
    - TLBI to update TLB
    - Resume the write
  Cost: 500–1000 cycles per write to a clean page

DBM (Dirty Bit Management, ARMv8.1):
  TCR_EL1.HD = 1 (requires HA=1)
  PTE bit[51] = DBM (Dirty Bit Management enable per page)
  
  When DBM=1 in PTE and AP[2]=1 (hardware sees as "writable+DBM"):
    First write: hardware changes AP[2] to 0 atomically in the page table
    Sets the page as "dirty" (AP[2]=0 means hardware considers page written)
    No fault taken!
    
  Cost: ~3–5 cycles (hardware PTE update during access)
  
  Linux interpretation:
    AP[2]=1 + DBM=1 → hardware dirty tracking enabled
    Hardware changes AP[2] to 0 on first write → page is now "dirty"
    Linux periodically checks AP[2] to detect dirty pages:
      AP[2]=1 still → page is clean (never written since last check)
      AP[2]=0 (changed by hardware) → page is dirty (write occurred)
    
  ARM64 Linux HA/HD detection and enablement:
    ID_AA64MMFR1_EL1.HAFDBS[3:0]:
      0b0000: AF and dirty bit managed by software only
      0b0001: Hardware AF management available (HA)
      0b0010: Hardware AF + dirty management available (HA + HD)
```

---

## 3. Linux HA/HD Integration

```
Linux source: arch/arm64/mm/hugetlbpage.c, arch/arm64/include/asm/pgtable.h

Detection (arch/arm64/kernel/cpufeature.c):
  arm64_has_hw_afdbm() → checks ID_AA64MMFR1_EL1.HAFDBS
  
  If HAFDBS >= 1: enable HA (set TCR_EL1.HA=1)
  If HAFDBS >= 2: enable HD (set TCR_EL1.HD=1)

PTE marking for hardware dirty tracking:
  pte_mkdirty_hw(pte):
    Sets DBM bit[51] in PTE
    Clears AP[2] (makes page logically read-only from SW perspective)
    BUT: DBM=1 means hardware WILL set AP[2]=0 when first written
    
  pte_dirty(pte):
    Returns true if AP[2]=0 (page has been written → dirty)
    With HW DBM: hardware changed AP[2] automatically on write
    
  pte_mkdirty(pte):
    Sets AP[2]=0 explicitly (software marks dirty, bypasses HW)
    
  pte_mkclean(pte):
    Sets AP[2]=1 (marks clean)
    With DBM: next write will hardware-dirty it again

Workflow for page writeback:
  1. Page is written (AP[2]=0 from DBM hardware)
  2. Writeback thread wants to write page to swap/file:
     pte_mkclean(pte) → AP[2]=1 (reset dirty)
     pte_mkwrite(pte) → ensure writable
     Write page to backing store
  3. After writeback: page is "clean" (AP[2]=1 again)
     If modified again: hardware sets AP[2]=0 → dirty again
  4. pte_dirty(pte) checks AP[2]:
     AP[2]=0 → page modified since last writeback → dirty=true
     AP[2]=1 → page not modified → dirty=false, can reclaim
```

---

## 4. Soft-Dirty Bit (userfaultfd and checkpointing)

```
Linux has an additional software-only "soft-dirty" bit for CRIU/checkpointing:
  PTE bit[55] = _PAGE_SOFTDIRTY (when used for soft-dirty tracking)
  (This is a software-defined bit, not ARM64-hardware-defined)
  
  Purpose: track which pages were written since the last checkpoint
  Used by CRIU (Checkpoint/Restore In Userspace) to save process state
  
  /proc/PID/pagemap bit[55]: soft-dirty bit for each page
  /proc/PID/clear_refs: clear soft-dirty bits for all pages
  
  Different from AF/DBM:
    AF/DBM: kernel page replacement algorithm (reclaim)
    Soft-dirty: userspace checkpointing and live migration
```

---

## 5. Interview Questions & Answers

**Q1: What is the difference between the Access Flag (AF) and the Dirty Bit (DBM), and why does hardware management matter?**

**Access Flag (AF, bit[10])** tracks whether a page has been READ or WRITTEN since the AF was last cleared. It's used by the kernel's page replacement algorithm (LRU/second-chance) to identify "cold" pages suitable for reclaiming. Without HA (hardware AF management), every first access to a page with AF=0 causes a fault: the CPU takes an exception, the Linux fault handler sets AF=1 in the PTE, performs a TLBI, and resumes — costing ~500–1000 cycles per page access.

**Dirty Bit / DBM (bit[51])** tracks whether a page has been WRITTEN specifically. This is needed for writeback: the kernel must write modified pages to their backing store (swap, file) before freeing them. Without HD (hardware dirty management), Linux marks pages read-only, takes a write fault on first write, software sets the page dirty, and makes it writable — again ~500–1000 cycles per write.

**Hardware management matters because** these faults happen on EVERY first access to a new page: new virtual pages, after page replacement resets the flags, and after mprotect operations. On a server with a database loading 100GB of data, hardware AF management can save billions of fault cycles during the initial load. Linux alternative-patches to use HA/HD when `ID_AA64MMFR1_EL1.HAFDBS` indicates support, providing 100–300× speedup for page-intensive workloads.

---

## 6. Quick Reference

| Bit | Name | HW managed? | Purpose |
|---|---|---|---|
| PTE[10] | AF (Access Flag) | Yes (HA=1 in TCR) | Track page access (LRU) |
| PTE[7] = AP[2] | Dirty (via DBM) | Yes (HD=1 in TCR, DBM=1 in PTE) | Track page modification |
| PTE[51] | DBM enable | N/A | Enable HW dirty for THIS page |

| TCR bit | Function | Required by |
|---|---|---|
| HA [39] | Hardware AF | ID_AA64MMFR1_EL1.HAFDBS >= 1 |
| HD [40] | Hardware dirty (DBM) | ID_AA64MMFR1_EL1.HAFDBS >= 2, also HA=1 |

| Feature | SW overhead | HW overhead |
|---|---|---|
| AF=0 → fault → set AF=1 | 500–1000 cycles | N/A |
| HA: hardware sets AF | N/A | 3–5 cycles |
| First write fault → set dirty | 500–1000 cycles | N/A |
| HD+DBM: hardware sets dirty | N/A | 3–5 cycles |
