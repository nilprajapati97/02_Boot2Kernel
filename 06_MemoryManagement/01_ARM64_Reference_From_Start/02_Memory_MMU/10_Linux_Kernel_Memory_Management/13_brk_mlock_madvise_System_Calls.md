# brk, mlock, and madvise System Calls

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. brk() — Heap Management

```
brk(): the traditional system call for managing the heap
  Controls the "program break" — the boundary of the heap VMA
  [mm->start_brk, mm->brk) = the heap VMA range
  
  malloc() (glibc): uses brk() for small allocations (below MMAP_THRESHOLD=128KB)
                    uses mmap(MAP_ANONYMOUS) for large allocations
  
  Prototype:
    int brk(void *addr);     // set heap end to addr
    void *sbrk(intptr_t);   // legacy: extend by N bytes (NOT a syscall)
  
  Linux: sbrk() is implemented in libc using brk()
  
  Behavior:
    brk(addr > current brk):  EXTEND heap (grow up)
    brk(addr < current brk):  SHRINK heap (free heap pages)
    brk(NULL) or brk(0):      Query current heap end (return current mm->brk)

Call chain:
  sys_brk(unsigned long brk)
    ↓
    mm = current->mm
    mmap_write_lock(mm)
    
    // Validate request:
    if (brk < mm->start_brk): goto out  // can't go below heap start
    
    newbrk = PAGE_ALIGN(brk)
    oldbrk = PAGE_ALIGN(mm->brk)
    
    if (newbrk == oldbrk):
      // No change in page boundary, just update mm->brk
      mm->brk = brk
      goto out
    
    if (newbrk > oldbrk):  // Extend heap
      // Check: would new heap VMA be too large?
      // Check: RLIMIT_DATA (max data segment size)
      if check_data_rlimit(newbrk - mm->start_data, ...): goto out
      
      do_brk_flags(oldbrk, newbrk - oldbrk, 0, NULL):
        // Extend or create heap VMA:
        // If existing heap VMA can be extended: just update vm_end
        // Else: create new VMA with VM_READ|VM_WRITE
        // arm64: vm_page_prot = PAGE_COPY (user RW, no exec, UXN=1)
        mm->brk = brk
    
    else:  // Shrink heap
      do_munmap(mm, newbrk, oldbrk - newbrk, NULL):
        // Remove PTEs and VMA for [newbrk, oldbrk)
        // TLB flush for unmapped range
        // Physical pages freed (if refcount drops to 0)
      mm->brk = brk
    
    mmap_write_unlock(mm)
    return brk (new heap end)

malloc() interaction with brk():
  glibc malloc: manages a heap using brk() at the lowest level
  First malloc: brk() extends heap by initial chunk (typically 128KB)
  Small subsequent mallocs: glibc carves from its internal heap
  When heap exhausted: brk() again to extend
  free(): glibc manages internal free lists; only calls brk() to trim
          the heap if the top of the heap is free (MORECORE_CONTIGUOUS)
  
  Large allocations (> MMAP_THRESHOLD = 128KB):
    glibc calls mmap(MAP_ANONYMOUS, MAP_PRIVATE)
    free() for these: calls munmap()
```

---

## 2. mlock() — Page Locking

```
mlock(): lock pages into RAM, preventing them from being swapped out
  Useful for: security (encryption keys not to disk), real-time (no latency spikes)
  Marked: vm_flags |= VM_LOCKED; PG_mlocked on each page
  
  Cost: pages CANNOT be reclaimed, even under memory pressure
  Risk: wrong use causes OOM (kernel kills processes)
  
  Requires: CAP_IPC_LOCK privilege OR within RLIMIT_MEMLOCK limit

Prototype:
  int mlock(const void *addr, size_t len);
  int mlock2(const void *addr, size_t len, unsigned int flags);
    // flags = MLOCK_ONFAULT: lock pages as they are faulted in (not immediately)

Call chain:
  sys_mlock(addr, len)
    ↓
    do_mlock(addr, len, VM_LOCKED)
      ↓
      mmap_write_lock(mm)
      
      // Find all VMAs in [addr, addr+len)
      // For each VMA: mlock_fixup(vma, &prev, addr, end, newflags)
      
      mlock_fixup(vma, prev, addr, end, newflags):
        if (newflags & VM_LOCKED && !(vma->vm_flags & VM_LOCKED)):
          // Adding lock to this VMA
          vma->vm_flags |= VM_LOCKED
          
          // Populate (fault-in) all pages in range:
          mm_populate(addr, end - addr):
            __get_user_pages():
              // Walk page tables: for each page not present:
              //   handle_mm_fault() → fault in the page
              //   Result: page is now in RAM and has PTE
              mlock_vma_page(page, vma, compound):
                // Set PG_mlocked on the page
                SetPageMlocked(page)
                // Add to unevictable LRU (cannot be reclaimed):
                if (!isolate_lru_page(page))
                  putback_lru_page() → goes to unevictable list
        
        else if (!(newflags & VM_LOCKED) && (vma->vm_flags & VM_LOCKED)):
          // Removing lock
          vma->vm_flags &= ~VM_LOCKED
          munlock_vma_pages_range(vma, addr, end):
            // For each page: clear PG_mlocked
            // page_evictable(page): check if now evictable
            // If evictable: move from unevictable LRU to active LRU
      
      mmap_write_unlock(mm)

munlock() / munlockall():
  Reverse of mlock: clear VM_LOCKED, move pages to evictable LRU
  
mlockall(MCL_CURRENT | MCL_FUTURE):
  MCL_CURRENT: lock all CURRENTLY mapped pages
  MCL_FUTURE: lock all FUTURE mmap/brk allocations too (until munlockall)
```

---

## 3. madvise() — Memory Access Advice

```
madvise(): hint to kernel about expected access pattern for a memory region
  Kernel may optimize: readahead, reclaim policy, NUMA placement, THP
  
Prototype:
  int madvise(void *addr, size_t len, int advice);

Important advice values:

MADV_NORMAL:
  Default behavior (remove other advice)

MADV_SEQUENTIAL:
  Expect sequential access → aggressive readahead
  Kernel: ra_state.ra_pages = max (doubles readahead window)

MADV_RANDOM:
  Expect random access → disable readahead
  Kernel: vma->vm_flags |= VM_RAND_READ
  Kernel: ra_state.ra_pages = 0 (no readahead)

MADV_WILLNEED:
  Expect to access soon → prefault pages
  Kernel: fadvise_willneed() → file_map_willneed()
    → force_page_cache_ra(): schedule readahead now
    → pages faulted in before process accesses them (reduces latency)

MADV_DONTNEED:
  No longer needed → kernel may free pages (and PTEs!)
  IMPORTANT: clears page tables, pages freed immediately if not locked
  Next access: page fault (demand paging again)
  
  Implementation: do_madvise_dontneed():
    zap_page_range(vma, addr, len):
      ptep_clear_flush_young() for each PTE
      ptep_clear_flush() to remove PTE
      page_remove_rmap() for each page
      put_page() → may free page
    
  GOTCHA: MADV_DONTNEED frees anonymous page content!
    If process later reads: gets fresh zero pages (not previous content)
    Use case: glibc free() for large anonymous allocations

MADV_FREE (Linux 4.5+):
  Lazy MADV_DONTNEED: kernel MAY free pages under memory pressure
  But: if process writes to the page BEFORE kernel reclaims it:
    page is NOT freed (content preserved)
  More efficient than MADV_DONTNEED for malloc() free path
  ARM64: PTE still valid initially; kernel marks page as "lazyfree"
         (PG_swapbacked cleared, but page still mapped)

MADV_HUGEPAGE:
  Encourage THP (transparent huge pages) for this VMA
  Sets VM_HUGEPAGE flag
  khugepaged may promote existing 4KB page cluster to 2MB THP

MADV_NOHUGEPAGE:
  Prevent THP for this VMA
  Sets VM_NOHUGEPAGE flag

MADV_MERGEABLE / MADV_UNMERGEABLE:
  Enable/disable KSM for this VMA
  MERGEABLE: register with ksmd for same-page merging
  UNMERGEABLE: unregister, un-merge any merged pages

MADV_DONTDUMP:
  Exclude VMA from core dumps
  Sets VM_DONTDUMP flag
  Useful for: JIT code caches, sensitive data

MADV_DOFORK / MADV_DONTFORK:
  DONTFORK: don't copy this VMA in fork() (child won't see it)
  DOFORK: reverse (default)
  Use: pinned DMA buffers that should not be inherited by child

MADV_POPULATE_READ / MADV_POPULATE_WRITE (Linux 5.14+):
  Like MAP_POPULATE but on existing mapping:
  POPULATE_READ: fault-in all pages for reading
  POPULATE_WRITE: fault-in all pages for writing (triggers CoW)
```

---

## 4. ARM64-Specific Considerations

```
brk() on ARM64:
  Heap grows UP (increasing virtual addresses)
  ARM64 ASLR: start_brk is randomized slightly above end of BSS
  Heap VMAs: VM_READ | VM_WRITE, UXN=1 (no execute on heap)
    This is the W^X (Write XOR Execute) policy:
    heap is writable but not executable
    ARM64: UXN=1 in PTE (user execute never) for heap pages
    Any attempt to execute heap: instruction abort fault → SIGSEGV

mlock() on ARM64:
  RLIMIT_MEMLOCK: default 64KB (too low for many realtime apps)
  /etc/security/limits.conf: increase for privileged processes
  ARM64 server use (Neoverse): often increase to unlimited for databases
  
  ARM64 and WFE:
    When mlock() is faulting in many pages: may compete with page cache
    ARM64 WFE used in lock contention paths (mmap_lock)
  
madvise(MADV_WILLNEED) and ARM64:
  Prefault: handle_mm_fault() called for each page
  ARM64: instruction fetch units benefit from MADV_WILLNEED for
         code pages (reduces I-cache miss storms at process start)

MADV_DONTNEED and ARM64:
  After zap_page_range(): must flush TLB
  ARM64: TLBI ASIDE1IS (flush all entries for this ASID)
         or individual TLBI VAE1IS per page (if range is small)
  DSB ISH to complete invalidation before returning to user
```

---

## 5. Interview Questions & Answers

**Q1: What is the difference between MADV_DONTNEED and MADV_FREE? When would you use each?**

**MADV_DONTNEED**: immediately removes pages. The kernel calls `zap_page_range()` which clears all PTEs and may free the physical pages right away. The next access will be a page fault getting fresh zero pages. Content is **immediately gone**.

**MADV_FREE** (Linux 4.5+): lazily marks pages as "may be reclaimed." The pages remain mapped (PTEs intact) with a "lazyfree" mark. Two scenarios:
1. Memory pressure: kernel reclaims the lazyfree pages → next access gets fresh zero pages
2. Process writes to a lazyfree page BEFORE reclaim: the "lazyfree" mark is cleared (page becomes normal) → page content is preserved → no page fault needed

**When to use each**:
- **MADV_DONTNEED**: when you KNOW you won't need the data again AND you want memory freed immediately (e.g., explicit `free()` equivalent for large anonymous buffers). Also useful for security: zero-out sensitive data, then `MADV_DONTNEED` to ensure it's not on swap.
- **MADV_FREE**: better for `malloc()`/`free()` patterns where the application might reallocate the same memory region shortly after freeing. MADV_FREE avoids the fault overhead if the memory is reused quickly. Used by glibc `free()` for large chunks.

**Practical difference**: 
- After `MADV_DONTNEED`: read → page fault, zero page
- After `MADV_FREE` + no memory pressure: read → NO fault (page still mapped), gets zero page only if it was never written

---

## 6. Quick Reference

| System Call | Primary Use | mmap_lock | Physical Pages Freed? |
|---|---|---|---|
| brk(extend) | Grow heap | Write | No (demand paged) |
| brk(shrink) | Shrink heap | Write | Yes (munmap range) |
| mlock() | Pin pages in RAM | Write | No |
| munlock() | Allow swapping again | Write | No |
| madvise(DONTNEED) | Free pages immediately | Write | Yes |
| madvise(FREE) | Lazy free | Read | Eventually (under pressure) |
| madvise(WILLNEED) | Prefault pages | Read | No |
| madvise(HUGEPAGE) | Enable THP | Write | No (restructured) |

| madvise Advice | vm_flags Change | Behavior |
|---|---|---|
| MADV_SEQUENTIAL | VM_SEQ_READ | Max readahead |
| MADV_RANDOM | VM_RAND_READ | No readahead |
| MADV_HUGEPAGE | VM_HUGEPAGE | THP eligible |
| MADV_NOHUGEPAGE | VM_NOHUGEPAGE | THP disabled |
| MADV_MERGEABLE | VM_MERGEABLE | KSM enabled |
| MADV_DONTDUMP | VM_DONTDUMP | Excluded from coredump |
| MADV_DONTFORK | VM_DONTCOPY | Not copied at fork |
