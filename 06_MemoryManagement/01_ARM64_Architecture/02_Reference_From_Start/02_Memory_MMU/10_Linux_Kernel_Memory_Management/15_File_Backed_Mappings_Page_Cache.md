# File-Backed Mappings and the Page Cache

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
File-backed mapping: virtual memory backed by a file on disk
  Created by: mmap(fd, prot, MAP_SHARED/MAP_PRIVATE, offset, len)
  Also: ELF loading (kernel maps .text, .data, .rodata from ELF file)
  
  Physical pages: from the PAGE CACHE
  Page cache: in-memory cache of file data
    Every file read/write on Linux goes through the page cache
    "read()" copies from page cache → user buffer
    "write()" copies user buffer → page cache (dirty page)
    mmap(MAP_SHARED): DIRECT mapping of page cache pages (zero-copy!)

Two types of file-backed mappings:
  MAP_SHARED:
    Process PTEs point directly to page cache pages
    Writes: dirty the page cache page → written back to disk later
    Other processes with MAP_SHARED on same file: see each other's writes
    Used for: IPC, shared libraries (read-only MAP_SHARED of .so files)
    
  MAP_PRIVATE:
    Initially: PTEs point to page cache pages (read-only, CoW)
    Read: reads from page cache (shared, no copy)
    Write: CoW! Allocate new anonymous page, copy from page cache
    The new page is PRIVATE to this process (NOT in page cache)
    Used for: ELF .data loading, private file mappings, copy-on-demand

Page cache:
  Core structure: struct address_space (in struct inode->i_mapping)
  Storage: XArray (since 5.7) of struct page pointers, indexed by file offset
  
  struct address_space {
    struct inode         *host;        // file this cache belongs to
    struct xarray         i_pages;     // XArray of cached pages
    atomic_t              i_mmap_writable; // count of writable MAP_SHARED
    struct rb_root_cached i_mmap;      // interval tree of VMAs mapping this file
    unsigned long         nrpages;     // number of pages in cache
    const struct address_space_operations *a_ops;  // readpage, writepage, etc.
    spinlock_t            private_lock; // protects private data
  };
```

---

## 2. File-Backed Page Fault: do_fault()

```c
/* mm/memory.c */

static vm_fault_t do_fault(struct vm_fault *vmf)
{
    if (!(vmf->flags & FAULT_FLAG_WRITE)) {
        return do_read_fault(vmf);      // read from file
    } else if (!(vmf->vma->vm_flags & VM_SHARED)) {
        return do_cow_fault(vmf);       // private write → CoW from file
    } else {
        return do_shared_fault(vmf);    // shared write → dirty page cache
    }
}

/* Read fault: load page from file into page cache, map it */
static vm_fault_t do_read_fault(struct vm_fault *vmf)
{
    vm_fault_t ret;
    
    // Map pages ahead (readahead):
    if (vmf->vma->vm_ops->map_pages && fault_around_bytes >> PAGE_SHIFT > 1)
        do_fault_around(vmf);  // map_pages() for N pages at once (readahead)
    
    ret = __do_fault(vmf);
    // → vma->vm_ops->fault(vmf)
    //   For ext4/xfs: filemap_fault():
    //     1. find_get_page(mapping, pgoff): lookup page in page cache XArray
    //     2. If found and PG_uptodate: minor fault (already in cache)
    //     3. If not found: page_cache_alloc_readahead() → allocate page
    //                      add_to_page_cache_lru(page, mapping, pgoff)
    //                      filemap_read_page() → submit disk I/O
    //                      → lock_page(page) + submit_bio() + MAJOR FAULT
    //                      → wait for I/O: lock_page() sleeps until done
    //                      → SetPageUptodate(page)
    
    if (!(ret & VM_FAULT_ERROR))
        ret |= finish_fault(vmf);
        // finish_fault():
        //   entry = mk_pte(vmf->page, vma->vm_page_prot)
        //   set_pte_at(mm, vmf->address, pte, entry)
        //   ARM64: entry = PA | AP bits | AttrIndx | UXN | valid
        //   AP based on prot: read-only for MAP_PRIVATE first access
    
    return ret;
}

/* Private write (CoW from file): */
static vm_fault_t do_cow_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    vm_fault_t ret;
    
    // Allocate new anonymous page (will receive copy of file content):
    vmf->cow_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, vmf->address);
    if (!vmf->cow_page) return VM_FAULT_OOM;
    
    // Load page from file into vmf->page (page cache):
    ret = __do_fault(vmf);
    if (ret & VM_FAULT_ERROR) goto out;
    
    // Copy file content to private page:
    copy_user_highpage(vmf->cow_page, vmf->page, vmf->address, vma);
    // ARM64: copy_page() → 4KB memcpy using LDNP/STNP (non-temporal pairs)
    
    // Set page as private anonymous (not page cache):
    __SetPageUptodate(vmf->cow_page);
    
    // Install PTE for the new private page (writable):
    ret |= finish_fault(vmf);  // installs vmf->cow_page as PTE, writable
    
    return ret;
}

/* Shared write: dirty the page cache page */
static vm_fault_t do_shared_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    vm_fault_t ret;
    
    ret = __do_fault(vmf);  // load from file (or find in cache)
    if (ret & VM_FAULT_ERROR) return ret;
    
    // Notify file system that page will be dirtied:
    if (vma->vm_ops->page_mkwrite) {
        unlock_page(vmf->page);
        ret = do_page_mkwrite(vmf);
        // For journaling filesystems (ext4 with journaling):
        //   page_mkwrite reserves journal space
        //   If journal full: SIGBUS or block
        if (ret & VM_FAULT_ERROR) goto out;
    }
    
    // Install PTE as writable:
    finish_fault(vmf);
    // ARM64: PTE with AP=EL0_RW (AP[2]=0)
    
    fault_dirty_shared_page(vmf);
    // Mark page dirty: SetPageDirty(page)
    // Mark inode dirty: mark_inode_dirty(inode)
    // Background writeback will flush to disk eventually
    
    return ret;
}
```

---

## 3. Page Cache and Readahead

```
Page cache: the kernel's buffer cache for ALL file data

  struct inode → i_mapping (struct address_space)
                 XArray i_pages: {pgoff → struct page*}
  
  Looking up a page:
    xa_load(&mapping->i_pages, offset):
      Returns page* if cached, NULL if not in cache
    
  Finding a page for mmap/file fault:
    filemap_get_folio(mapping, offset):
      Similar, but returns folio (possibly compound page)

Readahead:
  Problem: disk I/O has high latency (seek + rotational or PCIe overhead)
  Solution: predict future accesses and pre-read pages in advance
  
  Readahead state:
    struct file_ra_state in struct file:
      start:   first page of readahead window
      size:    current readahead window size (pages)
      async_size: async readahead threshold
      ra_pages: max readahead pages (from VMA flags)
  
  Readahead algorithm (ondemand_readahead()):
    Sequential access pattern detected:
      window doubles each time: 4 → 8 → 16 → 32 pages (up to 128KB default)
      page_cache_ra_unbounded(): submit read for the next `window` pages
      Pages added to page cache with PG_readahead flag on last page
    
    Random access (MADV_RANDOM): no readahead
    MADV_WILLNEED: forced readahead (fadvise)
  
  ARM64 benefit:
    ARMv8 hardware prefetcher handles spatial locality within cache lines
    Readahead is for bringing pages from disk/NVM into DRAM (coarser level)
    readahead and I-cache prefetch (PRFM) are complementary

Page cache eviction:
  LRU lists (per-NUMA-zone):
    Active list: recently accessed pages (warm)
    Inactive list: not recently accessed (cold, candidates for eviction)
  
  Balance: ratio of active:inactive managed by balance_pgdat()
  
  File pages evicted: NOT written to swap
    If PG_dirty: must write back to file first (writeback)
    If clean: just discarded (file is the backing store)
    Re-access: triggers file fault again → readahead from file
```

---

## 4. Shared Libraries: The Ultimate File-Backed Mapping Example

```
ELF shared library (.so) loading:
  ALL processes using libc.so share the SAME physical page cache pages!
  
  libc.so: 2MB of code pages (.text)
  100 processes using libc: 100 VMAs mapped, but only ~2MB of physical RAM
  (all 100 VMAs map to the SAME physical page cache pages for libc text)
  
  /proc/pid/maps:
    7f1234000000-7f1234200000 r-xp 00000000 08:01 12345678 /lib/libc.so.6
  
  .text section: MAP_SHARED, PROT_READ | PROT_EXEC
    PTE: AP=EL0_RO (read-only), UXN=0 (execute allowed)
    ARM64: UXN=0 means user-mode execution permitted
    All 100 processes: PTEs pointing to SAME physical pages
  
  .rodata section: MAP_SHARED, PROT_READ
    PTE: AP=EL0_RO, UXN=1 (no execute)
  
  .data section: MAP_PRIVATE, PROT_READ | PROT_WRITE
    Initially: PTE points to page cache page for .data (read-only CoW)
    First write to .data: CoW → private anonymous page per process
    After CoW: 100 processes × private .data page = more RAM used

i_mmap interval tree:
  address_space->i_mmap: tracks all VMAs mapping this file
  Needed for: file truncation (must unmap all VMAs that extend beyond new size)
            : MADV_DONTNEED of file-backed range (unmap and reload)
            : mmap writes that exceed file size (SIGBUS)
  
  vma_interval_tree_insert(vma, &mapping->i_mmap):
    Called at mmap() for file-backed VMA
  vma_interval_tree_remove(vma, &mapping->i_mmap):
    Called at munmap() for file-backed VMA
```

---

## 5. Interview Questions & Answers

**Q1: Two processes open the same file with MAP_SHARED. Process A writes to the mapping. Does process B immediately see the change?**

Yes, with caveats on ARM64.

Both processes' PTEs point to the **same physical page cache pages**. When process A writes, it modifies the physical page directly (the PTE allows write access). The page becomes dirty.

**Process B seeing the change** depends on **memory ordering**:
- The **physical page content** is updated immediately when process A writes
- If process B reads the SAME cache line as process A just wrote, and they're on the **same CPU or inner-shareable domain** (which is true on most ARM64 systems — inner-shareable covers all CPUs), the coherency hardware ensures B sees A's write
- ARM64 Normal memory is cache-coherent within the inner-shareable domain (DSH = 0b11 in PTE SH bits)
- However, **reordering** can mean B doesn't see A's write in a guaranteed order without barriers

**In practice**: For process-to-process IPC via MAP_SHARED, you still need synchronization:
- `msync(MS_SYNC)` or `msync(MS_ASYNC)`: not needed for coherency (cache coherent), but needed for ordering
- `smp_mb()` or `DMB ISH` barriers: to enforce ordering between write and the signal/notification that tells B to read

The **dirty page** in the page cache will be written back to disk by `pdflush`/`writeback` eventually, or immediately on `msync(MS_SYNC)`.

---

## 6. Quick Reference

| Mapping Type | PTE Initially | First Write | Page Cache? |
|---|---|---|---|
| MAP_SHARED (read) | RO, pointing to page cache | Makes PTE dirty | Yes |
| MAP_SHARED (write) | RW, pointing to page cache | Dirty in cache | Yes |
| MAP_PRIVATE (read) | RO, pointing to page cache | CoW: new anon page | Read-only from cache |
| MAP_PRIVATE (write) | RO then CoW | CoW: alloc anon page | No (after CoW) |
| Anonymous | Not mapped | Alloc + zero fill | Never |

| address_space Field | Purpose |
|---|---|
| i_pages (XArray) | Maps file offset → struct page |
| i_mmap (rb_root) | Maps file range → VMAs (for truncate, etc.) |
| nrpages | Total cached pages (for reclaim accounting) |
| a_ops->readpage | Read a page from disk (ext4, xfs) |
| a_ops->writepage | Write dirty page to disk |
| a_ops->page_mkwrite | Notified before first write to shared page |
