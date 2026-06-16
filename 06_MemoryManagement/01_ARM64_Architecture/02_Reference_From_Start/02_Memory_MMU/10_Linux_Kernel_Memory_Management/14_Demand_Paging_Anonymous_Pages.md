# Demand Paging and Anonymous Pages

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept: Demand Paging

```
Demand paging: physical memory pages are allocated ONLY when first accessed,
  NOT when the virtual memory region is created.

Why demand paging?
  A process may mmap 10GB but only read 100MB in practice
  Without demand paging: 10GB of RAM would be allocated at mmap() time → wasteful
  With demand paging: only 100MB ever allocated → 99% RAM saved
  
  Also: fork() copies address spaces → without demand paging, fork = copy all pages
  With demand paging + CoW: fork creates page table entries, not page copies

How it works:
  1. mmap() / malloc() / brk(): create VMA descriptor (vm_area_struct)
     No physical page allocated. No PTE installed.
  
  2. Process accesses virtual address:
     CPU MMU: tries to translate VA → no PTE found (or PTE=0)
     ARM64: raises Translation fault (ESR_EL1.DFSC = 0b000011)
  
  3. Page fault handler: allocate physical page, install PTE
     Process resumes from the faulting instruction (re-executes transparently)
  
  4. Subsequent accesses to same page: TLB hit → no fault → fast

Types of demand paging:
  Anonymous demand paging: pages not backed by any file
    → First access: allocate zero-filled page
    → If evicted: goes to swap
  
  File-backed demand paging: pages backed by a file (mmap of regular file)
    → First access: read from file into page cache
    → If evicted: NOT swapped (data is in file), just evicted
    → Next access: re-read from file (page fault again, but data preserved)
```

---

## 2. Anonymous Page Fault Path: do_anonymous_page()

```c
/* mm/memory.c */

static vm_fault_t do_anonymous_page(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct page *page;
    pte_t entry;
    
    // 1. Ensure PTE table exists (may need to allocate pmd/pte table):
    if (pte_alloc(vma->vm_mm, vmf->pmd)) return VM_FAULT_OOM;
    
    // 2. Read fault (no FAULT_FLAG_WRITE): use shared zero page!
    if (!(vmf->flags & FAULT_FLAG_WRITE) && !mm_forbids_zeropage(vma->vm_mm)) {
        entry = pte_mkspecial(pfn_pte(my_zero_pfn(vmf->address),
                                       vma->vm_page_prot));
        // ARM64 zero page:
        // my_zero_pfn() returns the PFN of the global zero page
        // pte_mkspecial: marks PTE as special (PTE_SPECIAL bit)
        //   → page->_refcount not incremented (shared, never freed)
        // entry has: AP=EL0_RO (read-only), UXN, valid
        
        vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd,
                                        vmf->address, &vmf->ptl);
        if (!pte_none(*vmf->pte)) goto unlock;  // race: another CPU faulted first
        
        set_pte_at(vma->vm_mm, vmf->address, vmf->pte, entry);
        // ARM64: STR x_entry, [x_pte]  (64-bit write)
        
        pte_unmap_unlock(vmf->pte, vmf->ptl);
        return VM_FAULT_NOPAGE;  // handled, no major fault
    }
    
    // 3. Write fault (first write): allocate fresh zero page:
    page = alloc_zeroed_user_highpage_movable(vma, vmf->address);
    if (!page) return VM_FAULT_OOM;
    
    // ARM64 alloc_zeroed_user_highpage_movable:
    //   alloc_page(GFP_HIGHUSER_MOVABLE | __GFP_ZERO)
    //   GFP_HIGHUSER_MOVABLE: from user zone, movable (migration ok)
    //   __GFP_ZERO: page zeroed by clear_page()
    //   ARM64 clear_page():
    //     Uses DC ZVA (Data Cache Zero by VA) if DCZID_EL0.BS != 0xF
    //     DC ZVA zeros an entire cache block (64 bytes on Cortex-A57+)
    //     Loop: for each block in page: DC ZVA address
    //     Faster than memset() for L1-cold pages
    
    // 4. Set up reverse mapping:
    __page_set_anon_rmap(page, vma, vmf->address, 1);
    // = page->mapping = anon_vma | 0x1
    // = page->index   = linear_page_index(vma, vmf->address)
    //                  = (vmf->address - vma->vm_start) >> PAGE_SHIFT + vma->vm_pgoff
    
    // 5. Build PTE entry:
    entry = mk_pte(page, vma->vm_page_prot);
    entry = pte_sw_mkyoung(entry);    // set AF (access flag)
    if (vma->vm_flags & VM_WRITE)
        entry = pte_mkwrite(pte_mkdirty(entry));
    // ARM64 pte_mkwrite: clears AP[2] bit (makes PTE writable)
    // ARM64 pte_mkdirty: sets software dirty bit[55]
    
    // 6. Install PTE:
    vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd,
                                    vmf->address, &vmf->ptl);
    if (!pte_none(*vmf->pte)) {
        // Race: another thread faulted same page
        put_page(page);
        goto unlock;
    }
    
    set_pte_at(vma->vm_mm, vmf->address, vmf->pte, entry);
    // ARM64: STLR (store-release) or STR depending on context
    
    update_mmu_cache(vma, vmf->address, vmf->pte);
    // ARM64: no-op for installation (hardware will fill TLB on next access)
    //        but if replacing existing entry: TLBI VAE1IS
    
    pte_unmap_unlock(vmf->pte, vmf->ptl);
    return VM_FAULT_MINOR;
}
```

---

## 3. Zero Page Optimization

```
The zero page optimization:
  First READ fault to an anonymous page → map the SHARED ZERO PAGE
  (not a private page for this process)
  
  Why? The READ returns zeros anyway (uninitialized anonymous memory)
  All reads get the same zero content → ONE physical page can serve ALL reads
  
  ZERO_PAGE(addr): defined in arch/arm64/include/asm/pgtable.h
    = pfn_to_page(my_zero_pfn(addr))
    = the special zero page (physically zeroed 4KB page)
    ARM64: zero page PA typically at a fixed location set in head.S
    PFN = early_zero_page_pfn (set in setup_arch())

  Properties of zero page:
    struct page: _refcount = 1 (never freed, special)
    PTE for zero page: PTE_SPECIAL set → put_page() doesn't decrement refcount
    Read-only: AP=EL0_RO in PTE
    
  When first WRITE to zero page:
    ARM64: permission fault (AP=EL0_RO → write attempted by EL0)
    do_wp_page():
      → new page needed (zero page can NEVER be made writable)
      → alloc_zeroed_user_highpage_movable() → new zeroed page
      → install new PTE (writable, AP=EL0_RW)
    → effectively: first write allocates the real page
    This is called "zero page CoW"

Memory savings from zero page:
  Large process: 100MB of heap, only 1MB ever written
  Without zero page opt: 100MB of RAM (all faults allocate real pages)
  With zero page opt: ~1MB real pages + tiny cost for zero page PTEs
  Stack: most stack pages never written → zero page mapped, freed on cleanup
```

---

## 4. Physical Page Allocation for Anonymous Pages

```
alloc_zeroed_user_highpage_movable(vma, addr):
  = alloc_pages_vma(GFP_HIGHUSER_MOVABLE | __GFP_ZERO, 0, vma, addr, ...)
  
  GFP_HIGHUSER_MOVABLE breakdown:
    __GFP_USER:     allocation is for user space
    __GFP_HIGHMEM:  prefer ZONE_HIGHMEM if available (ARM64: no HIGHMEM usually)
    __GFP_MOVABLE:  page can be migrated (for memory compaction, NUMA balancing)
    __GFP_ZERO:     zero the page
    GFP_KERNEL:     base: can sleep, may reclaim, may OOM kill
  
  Page obtained from: buddy allocator (order-0 allocation = 1 × 4KB page)
  
  After allocation:
    page->flags: PG_uptodate set (content valid = zeros), PG_lru clear
    page->_refcount: 1 (allocation holds one reference)
    page->_mapcount: -1 (not yet mapped in any PTE)
  
  Then: page_add_new_anon_rmap() sets up anon_vma backlink
        and increments _mapcount to 0 (now mapped in 1 PTE)
  
  Then: set_pte_at() installs PTE
        Now: _mapcount = 0 (1 PTE mapping)
  
  If process forks:
        page_dup_file_rmap() → _mapcount = 1 (2 PTEs)
  
  If process exits:
        page_remove_rmap() → _mapcount = -1
        put_page() → _refcount = 0 → __free_pages() → buddy allocator gets it back

ARM64 page zeroing (clear_highpage / clear_page):
  arch/arm64/lib/clear_page.S:
    Uses DC ZVA (Data Cache Zero by VA) instruction
    Each DC ZVA clears DCZID_EL0.BS × 4 bytes (typically 64 bytes = one cache line)
    Loop for 4KB page: 4096/64 = 64 iterations
    DC ZVA also invalidates cache lines (avoiding dirty eviction)
    ~3–5× faster than naive memset() for large pages
    
    Requirements: DC ZVA requires write access (EL1)
    DCZID_EL0.DZP must be 0 (DC ZVA permitted at EL0) for user use
    Kernel always uses it (EL1 always permitted)
```

---

## 5. Stack Growth: Demand Paging for Stack

```
Stack is also demand-paged:
  Stack VMA: vm_flags = VM_READ|VM_WRITE|VM_GROWSDOWN
  Initial stack: just the VMA, minimal PTEs (typically just top page)
  
  Stack growth downward:
    Function call → SP decremented below current stack PTE
    → Translation fault (no PTE for that VA)
    → do_page_fault(): addr < vma->vm_start
    → VM_GROWSDOWN set → expand_stack(vma, addr):
        new_vm_start = addr & ~PAGE_MASK  (align down to page)
        if (vma->vm_start - new_vm_start > rlimit(RLIMIT_STACK)): SIGSEGV
        vma->vm_start = new_vm_start
        vma->vm_pgoff -= (vma->vm_start - old_vm_start) >> PAGE_SHIFT
      → Now addr is inside VMA
    → Continue fault handling: do_anonymous_page() allocates stack page
  
  Stack guard page:
    Between heap and stack: 1 page VMA gap (not mapped)
    Prevents stack from overflowing silently into heap
    Stack overflow: hits guard page → fault → no VMA found → SIGSEGV
    
    ARM64 stack layout for process:
      [0x0000_7FFF_FFFF_0000] ← stack starts (grows down)
      [0x0000_7FFF_FFFE_F000] ← guard page (no VMA)
      [0x0000_7FFF_FFFE_E000] ← heap top (brk, grows up)
    
  RLIMIT_STACK: maximum stack size (default 8MB)
    expand_stack() checks: total stack size ≤ RLIMIT_STACK
    If over: SIGSEGV (stack overflow)
    
  ulimit -s: view/set stack limit
  Thread stacks: allocated via mmap() (not brk-based)
    Typically: 8MB or 2MB per thread + guard page
    pthread_create(): mmap() for new thread stack
```

---

## 6. Interview Questions & Answers

**Q1: Why does reading from freshly malloc'd memory return zeros on Linux even though no zeroing is explicitly requested?**

There are two mechanisms:

**Anonymous zero page**: The first READ access to a new anonymous page maps the **shared zero page** (a single physical page that is always all zeros). Multiple processes reading from uninitialized heap memory all get this same zero page (read-only mapping). It returns zeros correctly.

**New page allocation with `__GFP_ZERO`**: When the first WRITE happens (triggering CoW and actual page allocation), the kernel allocates a new page with `__GFP_ZERO`. On ARM64, `clear_page()` uses `DC ZVA` to efficiently zero the 4KB page. So even after the write triggers allocation, the page starts zeroed.

**Security reason**: Linux zeros pages before giving them to user space to prevent **information leakage**. If page A was used by process X (with secrets), freed, then given to process Y — without zeroing, process Y could read process X's secrets by reading the "uninitialized" memory. The kernel guarantees all new pages are zero-filled.

**Performance note**: The zero page optimization means many read-only accesses to large heap areas (e.g., a big hash table that's mostly empty) consume virtually no physical RAM — all map to the same zero page — until actually written.

---

## 7. Quick Reference

| Scenario | Fault Type | Handler | Physical Page Allocated? |
|---|---|---|---|
| First READ to anonymous page | Translation fault | do_anonymous_page → zero page | No (shared zero page) |
| First WRITE to anonymous page | Translation fault | do_anonymous_page → alloc_page | Yes |
| Write to zero-page mapped page | Permission fault | do_wp_page → alloc_page | Yes |
| Stack growth downward | Translation fault | expand_stack + do_anonymous_page | Yes |
| Second access to same page | TLB hit | No fault | N/A |

| ARM64 Operation | Purpose |
|---|---|
| DC ZVA | Zero 64 bytes of cache (used in clear_page) |
| STLR | Store-release PTE (ensure ordering) |
| TLBI VAE1IS | Flush TLB for one VA after PTE install |
| LDAXR/STLXR | Atomic PTE updates (ptep operations) |
