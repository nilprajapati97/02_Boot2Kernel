# Copy-On-Write (CoW): fork() Implementation Deep Dive

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Copy-On-Write (CoW): optimization where parent and child share physical
  pages after fork(), making copies only when pages are WRITTEN.
  
Without CoW:
  fork() would duplicate ALL physical pages of the parent
  8 GB process → fork() needs 8 GB extra RAM immediately
  exec() after fork(): wasted all that copying (exec replaces address space)
  
With CoW:
  fork(): mark ALL writable pages read-only in BOTH parent and child
  Time: O(VMAs) to set up, not O(pages)
  Space: no new physical pages needed until first write
  
  exec() after fork() (most common usage):
    child calls exec() before writing any pages
    exec() replaces address space entirely
    CoW copy: ZERO pages copied → fork()+exec() is nearly free!

CoW principle:
  1. Parent has page P mapped writable
  2. fork(): P now mapped read-only in both parent AND child
  3. Either process writes to P:
     a. ARM64 hardware: permission fault (read-only PTE)
     b. Kernel: allocate new page P', copy P → P'
     c. Writer: now maps P' writable
     d. Other process: still maps P (unchanged)
  4. Both processes now have independent copies

CoW invariant:
  A page is read-only in PTE ONLY IF it is shared (mapcount > 1)
  OR it was just CoW'd but refcount hasn't been decremented yet
  Once mapcount drops to 1: page can be made writable again (wp_page_reuse)
```

---

## 2. fork() and copy_page_range()

```c
/* kernel/fork.c */
int copy_mm(unsigned long clone_flags, struct task_struct *tsk)
{
    struct mm_struct *mm, *oldmm;
    
    if (clone_flags & CLONE_VM) {
        // Thread: share the SAME mm_struct
        mmget(oldmm);
        tsk->mm = oldmm;
        return 0;
    }
    
    // Process fork: create NEW mm_struct with CoW page tables
    mm = dup_mm(tsk, current->mm);
    tsk->mm = mm;
    return 0;
}

struct mm_struct *dup_mm(struct task_struct *tsk, struct mm_struct *oldmm)
{
    struct mm_struct *mm = mm_alloc();
    
    // Copy mm_struct fields (limits, flags, etc.)
    memcpy(mm, oldmm, sizeof(*mm));
    mm_init(mm, tsk, mm->user_ns);
    
    // Key step: duplicate page tables
    dup_mmap(mm, oldmm):
        // For each VMA in parent:
        mas_for_each(&old_mas, vma, ULONG_MAX) {
            // Copy VMA struct
            new_vma = vm_area_dup(vma);
            vma_copy_policy(new_vma, vma);
            
            // Set up anon_vma for CoW tracking
            if (anon_vma_fork(new_vma, vma)) goto fail;
            
            // Copy page tables for this VMA
            copy_page_range(new_vma, vma):
                → copy_pgd_range()
                → copy_pud_range()
                → copy_pmd_range()
                → copy_pte_range()
        }
    return mm;
}
```

---

## 3. copy_pte_range(): Marking Pages CoW

```c
/* mm/memory.c */
static int copy_pte_range(struct vm_area_struct *dst_vma,
                           struct vm_area_struct *src_vma,
                           pmd_t *dst_pmd, pmd_t *src_pmd,
                           unsigned long addr, unsigned long end)
{
    pte_t *src_pte, *dst_pte;
    
    src_pte = pte_offset_map(src_pmd, addr);
    dst_pte = pte_alloc_map(dst_vma->vm_mm, dst_pmd, addr);
    
    do {
        pte_t pte = *src_pte;
        
        if (pte_none(pte)) {
            // No PTE: nothing to copy (demand page will install it later)
            continue;
        }
        
        if (!pte_present(pte)) {
            // Swapped out: copy the swap entry
            copy_swap_entry(dst_pte, src_pte, pte, dst_vma, src_vma, addr);
            continue;
        }
        
        // Present PTE: CoW setup
        page = pte_page(pte);
        
        if (likely(!is_cow_mapping(src_vma->vm_flags) || pte_write(pte))) {
            // Page is writable → must make BOTH parent and child read-only!
            ptep_set_wrprotect(src_vma->vm_mm, addr, src_pte);
            // ARM64: ptep_set_wrprotect():
            //   pte_val = pte_val(*src_pte) | PTE_RDONLY;  // set AP[2]=1
            //   set_pte_at(mm, addr, src_pte, __pte(pte_val));
            //   After changing parent PTE: must flush TLB for this address
            //   flush_tlb_page(src_vma, addr) → TLBI VAE1IS, addr
        }
        
        // Install same (now read-only) PTE in child page table:
        pte = pte_mkold(pte);   // clear AF bit (access flag) for child
        set_pte_at(dst_vma->vm_mm, addr, dst_pte, pte);
        
        // Update reverse mapping:
        page_dup_file_rmap(page, dst_vma, addr, false);
        // Increments page->_mapcount → now mapcount = 1 (2 processes)
        get_page(page);  // increment _refcount
        
    } while (src_pte++, dst_pte++, addr += PAGE_SIZE, addr != end);
}

/* ARM64 ptep_set_wrprotect detail:
   Before: PTE has AP[2]=0 (writable) → allow EL0 read/write
   After:  PTE has AP[2]=1 (read-only) → allow EL0 read only
   
   AP bits in ARM64 PTE[7:6]:
     AP=0b01: EL0 RW (user read-write)
     AP=0b11: EL0 RO (user read-only)
   
   Changing AP requires TLB flush:
     Old TLB entry: RW → process could write
     New TLB entry: RO → process gets permission fault on write
     Without flush: old TLB entry used → CoW not triggered!
*/
```

---

## 4. do_wp_page(): Handling the CoW Write Fault

```c
/* mm/memory.c */
static vm_fault_t do_wp_page(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct page *old_page;
    
    old_page = vm_normal_page(vma, vmf->address, vmf->orig_pte);
    
    if (PageAnon(old_page) && !PageKsm(old_page)) {
        // Anonymous page: check if we can reuse it
        
        if (page_count(old_page) == 1 && trylock_page(old_page)) {
            // ONLY reference is our mapping (no other holders)
            // OPTIMIZATION: just make PTE writable, no copy!
            return wp_page_reuse(vmf);
        }
    }
    
    // Need to copy: shared page (multiple mappers)
    return wp_page_copy(vmf);
}

static vm_fault_t wp_page_reuse(struct vm_fault *vmf)
{
    // Make the PTE writable in-place (no copy needed!)
    pte_t entry = pte_mkyoung(vmf->orig_pte);
    entry = maybe_mkwrite(pte_mkdirty(entry), vmf->vma);
    // ARM64: pte_mkwrite sets AP[2]=0 (RW)
    //        pte_mkdirty sets the dirty bit
    
    ptep_set_access_flags(vmf->vma, vmf->address, vmf->pte, entry, 1);
    // ARM64: set_pte_at + flush_tlb_page
    //   STLR to write new PTE
    //   TLBI VAE1IS, addr  + DSB ISH
    
    unlock_page(old_page);
    return VM_FAULT_WRITE;
}

static vm_fault_t wp_page_copy(struct vm_fault *vmf)
{
    struct mm_struct *mm = vmf->vma->vm_mm;
    struct page *old_page = vmf->page;
    struct page *new_page = NULL;
    
    // Allocate new page
    new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE | __GFP_ZERO, vmf->vma, vmf->address);
    if (!new_page) return VM_FAULT_OOM;
    
    // Copy old page content to new page
    cow_user_page(new_page, old_page, vmf);
    // ARM64 cow_user_page:
    //   copy_page(dst, src):
    //     Uses 64-byte LDNP/STNP (non-temporal pair) for efficiency
    //     Or SVE/NEON vectorized copy if kernel has it
    //   Then: DC CIVAC for each cache line? No!
    //   Actually: flush_dcache_page(new_page) if D-cache not coherent with I-cache
    //   ARM64: D+I cache coherent for kernel mappings (no flush needed for anon)
    
    // Remove old PTE from this VMA
    page_remove_rmap(old_page, vmf->vma, false);
    // Decrements old_page->_mapcount
    
    // Install new PTE
    pte_t entry = mk_pte(new_page, vmf->vma->vm_page_prot);
    entry = pte_sw_mkyoung(entry);
    entry = maybe_mkwrite(pte_mkdirty(entry), vmf->vma);
    // ARM64: entry has AP[2]=0 (writable), AF=1, dirty=1
    
    set_pte_at(mm, vmf->address, vmf->pte, entry);
    // ARM64: str x0, [x1] (64-bit PTE write)
    
    update_mmu_cache(vmf->vma, vmf->address, vmf->pte);
    // ARM64: TLBI VAE1IS after PTE change
    
    // Add reverse mapping for new page
    page_add_anon_rmap(new_page, vmf->vma, vmf->address, false);
    
    put_page(old_page);  // release reference to old page
    return VM_FAULT_WRITE;
}
```

---

## 5. anon_vma: CoW Tracking

```
anon_vma: tracks all VMAs that share a given anonymous page
  Needed for REVERSE MAPPING (rmap): page → all PTEs pointing to it
  Critical for: page reclaim (must unmap all PTEs before freeing page)

anon_vma hierarchy after fork:
  Parent VMA → anon_vma A
    fork() → child VMA → anon_vma B, BUT:
    child's anon_vma_chain links to parent's anon_vma A also
  
  Why? After fork, both parent and child map the same page
  The page's anon_vma pointer can only point to ONE anon_vma
  anon_vma_chain: linked list allowing one page to be found from
                  BOTH parent and child anon_vma via chain

anon_vma_fork() (during fork):
  1. Create new anon_vma for child
  2. Create anon_vma_chain linking child VMA to PARENT's anon_vma
  3. Create anon_vma_chain linking child VMA to CHILD's own anon_vma
  
  Result: page can be found from either parent or child's anon_vma tree

page_remove_rmap() (during CoW):
  Decrements old_page->_mapcount
  If _mapcount == -1: page has no more PTE mappings → can be freed

try_to_unmap() (page reclaim):
  Given a page: walk its anon_vma tree
  For each VMA in anon_vma tree: find PTE for this page in that VMA
  Clear PTE: ptep_clear_flush() → TLBI
  → After all PTEs cleared: page can be reclaimed (written to swap if dirty)
```

---

## 6. KSM (Kernel Same-page Merging)

```
KSM: after fork() or independent processes, pages with IDENTICAL content
  can be MERGED to save memory (reverse of CoW!)
  
KSM process:
  ksmd kernel thread: scans registered memory regions
  Compares page content (hash first, then byte-by-byte for matches)
  Merges identical pages:
    allocate KSM page (special struct page with KSM flag)
    remap all matching pages to the single KSM page
    mark all PTEs read-only
  
  When process writes to a KSM page: CoW! New page allocated.
  (Same mechanism as fork CoW, but triggered by KSM)

/sys/kernel/mm/ksm/:
  run: 0=stop, 1=run, 2=run+unmerge
  pages_shared: unique KSM pages (memory saved)
  pages_sharing: total pages merged (pages_sharing - pages_shared = savings)

ARM64 KSM CoW:
  Same do_wp_page() path
  KSM pages: PageKsm(page) = true
  wp_page_copy() always copies (never reuse KSM page in-place)
```

---

## 7. Interview Questions & Answers

**Q1: After fork(), what happens to the parent's page table entries? Are they immediately set to read-only?**

Yes, `copy_pte_range()` calls `ptep_set_wrprotect()` for EVERY writable PTE in the parent's page tables at fork time. After fork, BOTH parent and child have read-only PTEs for all previously writable pages.

This is one of fork's costs: it must scan all the parent's existing PTEs (the ones that are currently present — not the lazily-allocated ones) and write-protect them. For a large process with many mapped pages, this can take significant time.

The TLB flush is necessary after write-protecting the parent's PTEs: without flushing, the parent might still have cached writable TLB entries and could write without triggering a fault. ARM64 uses `TLBI VAE1IS, addr` (inner-shareable TLB invalidation by VA) for each page, or a full `TLBI ASID1IS` if the number of pages is large.

**Q2: What is the "one-page CoW optimization" (wp_page_reuse)?**

When `do_wp_page()` is called for a write to a read-only page, it first checks if this process is the **only** holder of the page (via `page_count == 1`). If the only reason the page was read-only was a previous fork, but the other process has since freed its mapping (or did a CoW itself), then the page's refcount will be 1.

In that case, there's no need to copy the page — we can simply **upgrade the PTE from read-only to read-write** in-place (`pte_mkwrite`). This avoids a physical page allocation and a 4KB memcpy, saving CPU time and memory bandwidth. The ARM64 operation is: write new PTE descriptor with `AP[2]=0` (writable), then `TLBI VAE1IS` to flush the old read-only TLB entry.

---

## 8. Quick Reference

| CoW Phase | Action | ARM64 Operation |
|---|---|---|
| fork() | Write-protect all present writable PTEs | `AP[2]=1` in PTE + `TLBI VAE1IS` |
| fork() | Copy PTE to child table | `set_pte_at()` → `STR` x64 |
| Write fault | Permission fault (read-only PTE written) | ESR.DFSC = 0b001111 |
| wp_page_reuse | Make PTE writable (page count==1) | `AP[2]=0` in PTE + `TLBI VAE1IS` |
| wp_page_copy | Allocate new page, copy content | `alloc_page` + `copy_page` (LDNP/STNP) |
| wp_page_copy | Install new PTE | `set_pte_at()` + `TLBI VAE1IS` |
| remove old map | Decrement old page mapcount | `page_remove_rmap()` |
