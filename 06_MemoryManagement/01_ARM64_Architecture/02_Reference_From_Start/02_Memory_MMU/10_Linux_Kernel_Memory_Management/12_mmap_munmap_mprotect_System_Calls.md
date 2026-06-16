# mmap, munmap, mprotect System Calls

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. mmap() System Call

```
mmap(): create a new virtual address mapping in the current process
  Creates a VMA (vm_area_struct) describing the mapping
  No physical pages allocated (lazy, demand paging)

Prototype:
  void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

  addr:   hint for VA to use (NULL = kernel chooses)
  length: size of mapping in bytes (rounded up to page boundary)
  prot:   PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE
  flags:  MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_POPULATE
  fd:     file to map (-1 for MAP_ANONYMOUS)
  offset: offset in file (multiple of page size)

prot → vm_flags:
  PROT_READ  → VM_READ  + VM_MAYREAD
  PROT_WRITE → VM_WRITE + VM_MAYWRITE
  PROT_EXEC  → VM_EXEC  + VM_MAYEXEC
  PROT_NONE  → no vm_flags (but VMA still exists, page fault = SIGBUS/SIGSEGV)

flags → vm_flags:
  MAP_SHARED  → VM_SHARED | VM_MAYSHARE
  MAP_PRIVATE → (no VM_SHARED — CoW mapping)
  MAP_ANONYMOUS → VM_ANONYMOUS (no file)
  MAP_GROWSDOWN → VM_GROWSDOWN (for stacks)
  MAP_LOCKED    → VM_LOCKED (mlock immediately)
  MAP_HUGETLB   → VM_HUGETLB
  MAP_POPULATE  → fault-in all pages immediately (like mlock without locking)
```

---

## 2. mmap() Kernel Call Chain

```
ARM64 syscall entry:
  SVC #0 → EL0→EL1 exception → syscall table lookup
  sys_mmap → ksys_mmap_pgoff()

mm/mmap.c call chain:
  ksys_mmap_pgoff(addr, len, prot, flags, fd, pgoff)
    ↓
    if (!(flags & MAP_ANONYMOUS)):
      file = fget(fd)   // get struct file from fd
    
    vm_mmap(file, addr, len, prot, flags, pgoff)
      ↓
      mmap_write_lock(mm)   // take mmap_lock WRITE (exclusive)
      do_mmap(file, addr, len, prot, flags, vm_flags, pgoff, ...)
        ↓
        get_unmapped_area(file, addr, len, pgoff, flags):
          // Find free VA range of size `len`
          // arch_get_unmapped_area_topdown() for ARM64:
          //   Searches downward from mmap_base (flexible layout)
          //   Returns VA that doesn't conflict with existing VMAs
          //   On ARM64: ASLR randomizes mmap_base per-process
          
        mmap_region(file, addr, len, vm_flags, pgoff, ucharged):
          ↓
          // Check and merge with existing adjacent VMAs
          vma_merge(mm, prev, addr, addr+len, vm_flags, ...):
            // Can merge if adjacent VMA has same flags/file/pgoff?
            // Merging saves VMA struct overhead
          
          if (!merged):
            vma = vm_area_alloc(mm)   // allocate new VMA struct
            vma->vm_start = addr
            vma->vm_end = addr + len
            vma->vm_flags = vm_flags
            vma->vm_page_prot = vm_get_page_prot(vm_flags)
            // ARM64: vm_page_prot derived from vm_flags using protection_map[]
            //   VM_READ | VM_WRITE → PAGE_COPY  (user RW, kernel RW, UXN)
            
            if (file):
              vma->vm_file = get_file(file)
              call_mmap(file, vma):
                → file->f_op->mmap(file, vma)
                // Sets vma->vm_ops (e.g., filemap_file_vm_ops for ext4)
                // May set VM_SHARED, etc.
            else (anonymous):
              vma->vm_ops = NULL (or &shmem_vm_ops for tmpfs-backed)
            
            vma_link(mm, vma):
              // Insert VMA into mm's maple tree + mm_count++
              mas_store(&mm->mm_mt, vma)
              mm->map_count++
            
            if (MAP_POPULATE):
              mm_populate(addr, len)
              // Force fault-in all pages (avoiding future faults)
          
      mmap_write_unlock(mm)
      return addr  // user gets back the mapped VA
```

---

## 3. munmap() System Call

```
munmap(): remove a virtual address mapping (or part of one)
  Frees VMAs, frees page tables, sends TLB flush
  Physical pages NOT freed here (they have separate reference counting)
  
  Physical page freed when: last PTE removed AND struct page refcount = 0

Call chain:
  sys_munmap(addr, len)
    ↓
    vm_munmap(addr, len)
      ↓
      mmap_write_lock(mm)
      do_munmap(mm, addr, len, NULL)
        ↓
        find_vma_intersection(mm, addr, addr+len)
        // Find all VMAs that overlap [addr, addr+len)
        
        if VMA starts before addr: split_vma() at addr
          // Create two VMAs from one: [vm_start, addr) and [addr, vm_end)
          // ARM64: no page table changes needed for split (PTEs already exist)
        
        if VMA ends after addr+len: split_vma() at addr+len
        
        // Now have exact set of VMAs covering [addr, addr+len)
        unmap_region(mm, vma, prev, addr, end):
          ↓
          unmap_vmas(tlb, mm, vma, addr, end):
            // For each VMA: unmap_single_vma()
            //   ptep_clear_flush() for each PTE
            //   ARM64: clear PTE + TLBI VAE1IS, addr
            //   page_remove_rmap() for each page
            //   put_page() → may free physical page if refcount hits 0
          
          free_pgtables(tlb, vma, floor, ceiling):
            // Free the actual page table PAGES
            // pte_free(), pmd_free(), pud_free()
            // flush_tlb_mm() or batched TLBI
          
          tlb_finish_mmu():  // Flush deferred TLB operations
            // ARM64: TLBI ASIDE1IS (by ASID) or individual VAE1IS
            // DSB ISH to wait for TLB invalidation completion
        
        remove_vma_list(mm, vma):
          // Remove VMAs from maple tree
          // mas_erase(&mas) for each
          // mm->map_count -= n_removed
          // vm_area_free() for each VMA struct
      
      mmap_write_unlock(mm)
```

---

## 4. mprotect() System Call

```
mprotect(): change permissions of an existing mapping
  Does NOT move/replace pages — just changes PTE protection bits
  Very fast for pages not yet mapped (just updates vm_flags)
  For already-mapped pages: must update existing PTEs + flush TLB

Call chain:
  sys_mprotect(addr, len, prot)
    ↓
    do_mprotect_pkey(addr, len, prot, pkey=-1)
      ↓
      mmap_write_lock(mm)
      
      for each VMA in [addr, addr+len):
        new_flags = calc_vm_prot_bits(prot, ...)
        // Convert PROT_* → VM_*
        
        if need to split: split_vma() at addr / addr+len
        
        mprotect_fixup(vma, &prev, addr, end, newflags):
          ↓
          // Update VMA flags:
          vma->vm_flags = newflags
          vma->vm_page_prot = vm_get_page_prot(newflags)
          
          // If trying to add write permission to file mapping:
          //   Check file has write permission (MAY_WRITE)
          
          // Update all existing PTEs in range:
          change_protection(vma, addr, end, vma->vm_page_prot, ...):
            ↓
            change_pte_range(mm, pmd, addr, end, newprot, ...):
              // For each present PTE:
              oldpte = ptep_modify_prot_start(mm, addr, pte)
              // ARM64: read PTE, clear it atomically
              
              newpte = pte_modify(oldpte, newprot)
              // ARM64: update AP bits, UXN bit, PXN bit
              //   pte_modify: (oldpte & _PAGE_CHG_MASK) | newprot
              //   AP bits:    newprot bits 7:6
              //   UXN bit:    newprot bit 54
              
              ptep_modify_prot_commit(mm, addr, pte, oldpte, newpte)
              // ARM64: set_pte_at() (write new PTE)
              // Then: flush_tlb_page(vma, addr)
              
          // Final TLB flush for whole range:
          flush_tlb_range(vma, addr, end)
          // ARM64: TLBI VAE1IS for each page OR TLBI ASIDE1IS
      
      mmap_write_unlock(mm)
```

---

## 5. mmap_lock Contention

```
mmap_lock (struct rw_semaphore) is the hot lock in memory management:
  Write lock (exclusive):
    mmap(): adding new VMA
    munmap(): removing VMA
    mprotect(): modifying VMA
    brk(): expanding heap
    execve(): replacing address space
  
  Read lock (shared):
    do_page_fault(): handling page faults
    /proc/pid/maps read: iterating VMAs
    ptrace: reading process memory
    
  Problem: page fault takes READ lock; mmap takes WRITE lock
    High thread count + frequent mmap/munmap → mmap_lock contention
    All threads doing page faults BLOCKED while mmap_write_lock held
    
  Linux solution (WIP): per-VMA locking (CONFIG_PER_VMA_LOCK)
    Each VMA has its own lock for page fault handling
    Page fault: takes VMA read lock (per-VMA, not mm-wide)
    mmap/munmap: still takes mm-wide write lock
    Reduces contention significantly for multi-threaded workloads

ARM64 mmap_lock (rw_semaphore):
  ARM64 uses generic Linux rw_semaphore (arch-independent)
  But: optimized with LSE LDSET/LDCLR atomics on ARMv8.1+
  WFE used for spinpath before sleeping
```

---

## 6. Interview Questions & Answers

**Q1: What happens internally when a process calls mprotect(PROT_NONE) on a range? What happens when it then accesses that memory?**

`mprotect(addr, len, PROT_NONE)` changes the VMA's `vm_flags` to have no `VM_READ|VM_WRITE|VM_EXEC` set. It then calls `change_protection()` which walks the existing PTEs in that range and modifies them.

For **already-mapped pages**: The ARM64 PTE is updated with `AP[1]=0` (user mode has no access at all — EL0 read/write denied) and `UXN=1` (no execute). A `TLBI VAE1IS` flushes the old TLB entries. The pages are NOT unmapped — they still physically exist, PTEs still point to them, but the access bits deny all access.

Special case: Linux uses `PTE_PROT_NONE` (a software bit in PTE) to mark pages that are mapped but access-denied (`PROT_NONE`). This distinguishes from a truly unmapped page (`pte_none`). The kernel needs this distinction to correctly handle faults on these pages.

When the process then **accesses** the PROT_NONE memory:
- ARM64 raises a **permission fault** (ESR.DFSC = permission fault, level 3)
- `do_page_fault()` finds the VMA (it still exists!)
- But `vma_is_accessible(vma)` returns false (no PROT_READ/WRITE/EXEC)
- `do_numa_page()` might handle it if it was a NUMA hint, otherwise:
- `bad_area_access_error()` → `force_sig_fault(SIGSEGV, SEGV_ACCERR, addr)`

This is used by tools like `valgrind` and guard pages: `mprotect(PROT_NONE)` after a buffer to catch buffer overflows — the overflow writes to the guard page → SIGSEGV → valgrind catches it.

---

## 7. Quick Reference

| System Call | VMA Operation | mmap_lock | TLB Impact |
|---|---|---|---|
| mmap() | Insert new VMA | Write lock | None (lazy) |
| munmap() | Remove VMA | Write lock | Full flush for removed range |
| mprotect() | Modify VMA flags | Write lock | Flush for present PTEs |
| brk() | Extend/shrink heap VMA | Write lock | Flush on shrink |
| mremap() | Move/resize VMA | Write lock | Flush old range |

| mmap() Flag | Effect |
|---|---|
| MAP_ANONYMOUS | No file backing (anonymous pages) |
| MAP_SHARED | Writes visible to all; file writeback |
| MAP_PRIVATE | CoW: writes private; no file writeback |
| MAP_FIXED | Use exact addr (unmaps anything at addr) |
| MAP_POPULATE | Fault-in all pages immediately |
| MAP_LOCKED | Like MAP_POPULATE + mlock (no swap) |
| MAP_HUGETLB | Use huge pages (2MB on ARM64) |
