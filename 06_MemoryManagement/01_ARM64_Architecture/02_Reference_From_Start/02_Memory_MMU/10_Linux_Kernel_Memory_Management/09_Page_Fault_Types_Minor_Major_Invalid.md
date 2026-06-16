# Page Fault Types: Minor, Major, Invalid, Protection, Instruction

**Category**: Linux Kernel Memory Management  
**Platform**: ARM64 (AArch64)

---

## 1. Overview of Page Fault Classification

```
Linux classifies page faults by cause and resolution:

                        Page Fault
                           │
            ┌──────────────┼──────────────────┐
            │              │                  │
         VMA found?      VMA found?        VMA found?
         fault in VMA    fault in hole     no VMA at all
            │              │                  │
         resolve it    expand stack?     → bad_area_nosemaphore
                       (if VM_GROWSDOWN)    → SIGSEGV
                           │
                        yes/no
                           
Resolution types (when VMA found, access matches permissions):
  1. Minor fault (demand paging): page not in memory, but not on disk
  2. Major fault (swap/file): page not in memory, need disk I/O
  3. Protection fault (CoW): page present but read-only, write attempted
  4. Instruction fault: execute denied (UXN=1 or PXN=1)
  5. NUMA fault: page migration to better NUMA node

Invalid access types (no VMA or wrong permissions):
  1. NULL pointer: address 0 (no VMA at address 0 by design)
  2. Stack overflow: below stack VMA limit
  3. Out of bounds: random address with no VMA
  → Send SIGSEGV to process
```

---

## 2. Minor Fault (Demand Paging)

```
Minor fault: page needs to be mapped but is ALREADY IN MEMORY
  (or zero page can be used)
  No disk I/O required → fast resolution
  Counted in: /proc/<pid>/stat field 10 (minor_faults)

Causes:
  a) Anonymous demand paging: first access to a new anonymous VMA
     (heap via malloc, stack growth, new mmap(MAP_ANON))
  b) CoW after fork: parent+child share page, first write → copy in RAM
  c) File-backed page already in page cache: page is in memory,
     just not mapped in this process's page table

ARM64 minor fault path (anonymous):
  do_anonymous_page():
    1. Check pte_alloc_map: ensure PTE table exists
    2. if (vmf->flags & FAULT_FLAG_WRITE && !in_cow):
         anon_page = alloc_zeroed_user_highpage_movable(vma, addr)
         // allocate fresh zeroed page
         // ARM64: calls clear_highpage(page) → DC ZVA if available
       else:
         page = ZERO_PAGE(0)   // shared read-only zero page (optimization!)
    3. entry = mk_pte(page, vma->vm_page_prot)
       if (write): entry = pte_mkwrite(entry)
    4. set_pte_at(mm, addr, pte, entry)
       // ARM64: str x0, [x1] (stores PTE descriptor, 64-bit)
       // Followed by DSB ISH + TLBI for invalidation if replacing old entry
    5. Return VM_FAULT_MINOR (no major fault bit set)

Read-only first access optimization (zero page):
  When reading an anonymous page for the first time:
  → map the shared ZERO_PAGE (physical page of all zeros, reference counted)
  → marked read-only in PTE
  → multiple processes reading from uninitialized memory share ONE physical page
  → When first write happens: permission fault → do_wp_page() → alloc new page
  
  Benefit: defer allocation until page is actually written
  ZERO_PAGE is never freed (special _refcount handling)
  ARM64 ZERO_PAGE: PA of a statically allocated zero-filled page (in __init_memblock)
```

---

## 3. Major Fault (Swap-In or File Read)

```
Major fault: page is NOT in memory, requires DISK I/O
  Slow: milliseconds (disk) vs microseconds (minor fault)
  Counted in: /proc/<pid>/stat field 12 (major_faults)
  Also: /proc/vmstat pgmajfault

Causes:
  a) Swap fault: anonymous page was evicted to swap partition/file
     PTE present=0, but pte has swap entry (special encoding)
  b) File-backed page fault: page was never read or was evicted from page cache
     → readahead from file system

ARM64 swap fault path:
  handle_pte_fault():
    → pte present? NO
    → pte_none? NO (has special swap entry encoding)
    → do_swap_page(vmf):
       
       1. entry = vmf->orig_pte;  // swap entry with swap slot encoded
          type = swp_type(entry);  // swap device index
          offset = swp_offset(entry);  // offset within swap
       
       2. swapcache_get_page(): look up in swap cache (page may be in memory!)
          if found: minor fault (page already back in memory, just remap)
       
       3. if not in swapcache:
            page = alloc_page(GFP_HIGHUSER_MOVABLE)
            swap_readpage(page, swap_entry):
              → disk I/O (submit_bio)
              → current thread blocks until I/O complete (lock_page + wait)
            → THIS IS THE MAJOR FAULT (disk I/O happened)
            current->maj_flt++
            set VM_FAULT_MAJOR return flag
       
       4. set_pte_at(): install new PTE with physical page
          pte_mkdirty() if was dirty before swapping
          swap_free(entry): release swap slot
       
       5. activate_page(page): move to active LRU

File-backed major fault path:
  do_fault() → do_read_fault():
    1. vma->vm_ops->fault(vmf):
       → filemap_fault() for regular files:
         a. find_get_page(mapping, pgoff): look in page cache
         b. if not in cache: page_cache_alloc() → add to cache
         c. filemap_read_page(file, mapping, page): submit readahead
            → sets PG_locked on page → I/O started → major fault!
         d. wait_on_page_locked(page): wait for I/O to complete
         e. SetPageUptodate(page)
    2. install PTE pointing to this page
    3. VM_FAULT_MAJOR returned

Readahead optimization:
  On major fault: kernel predicts future accesses (sequential pattern)
  page_cache_ra_unbounded(): read-ahead N pages in advance
  ARM64: submit_bio() → NVMe controller → DMA → cache pages filled
  Next access (sequential): minor fault (page already in cache)
```

---

## 4. Invalid Fault (SIGSEGV)

```
Invalid fault: access to unmapped/wrong-permission address
  → SIGSEGV sent to process
  Default SIGSEGV action: core dump + terminate

Causes:
  a) NULL pointer dereference: access to address 0 (no VMA)
  b) Use-after-free: VMA unmapped, pointer still used
  c) Buffer overflow: access beyond VMA boundary
  d) Stack overflow: stack grows past RLIMIT_STACK
  e) Arbitrary address: random VA with no VMA

Kernel handling (bad_area path):
  1. No VMA or VMA doesn't contain addr:
     → bad_area_nosemaphore()
     → __bad_area_nosemaphore()
     → fault_signal_pending(): check pending signals first
     → user fault: __do_user_fault(regs, esr)
       → force_sig_fault(SIGSEGV, SEGV_MAPERR, fault_addr)
     → kernel fault: __do_kernel_fault(mm, addr, esr, regs)
       → die() → panic() (kernel oops)
  
  2. VMA found but permissions wrong (no VM_WRITE but write attempted):
     → __do_user_fault(SIGSEGV, SEGV_ACCERR, ...)
     SEGV_ACCERR: access error (page exists but wrong permissions)
     SEGV_MAPERR: map error (page doesn't exist at all)

NULL pointer dereference protection:
  ARM64: address 0 is in user VA space (TTBR0 range)
  Kernel access to NULL in kernel mode: ESR_EL1.EC = data_abort_el1
  → __do_kernel_fault(): checks exception table
  → If fixup exists in exception table: jump to fixup
    (used by copy_from_user, __get_user, etc. with ARM64 extable)
  → No fixup: die() → BUG() → oops message → kernel panic (if panic_on_oops)

ARM64 Oops format:
  Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
  Mem abort info:
    ESR = 0x0000000096000004
    EC = 0x25: DABT (current EL), IL = 32 bits
    SET = 0, FnV = 0
    EA = 0, S1PTW = 0
    FSC = 0x04: level 0 translation fault
  [<ffffffc01234abcd>] bad_driver_func+0x40/0x80
```

---

## 5. Protection Fault (Write to Read-Only = CoW)

```
Protection fault: PTE present, access attempted that PTE doesn't allow
  Most common: write to read-only page (CoW)

ARM64 ESR: DFSC = 0b001111 (permission fault, level 3)

Protection fault path:
  handle_pte_fault():
    pte_present(entry) = true
    vmf->flags & FAULT_FLAG_WRITE = true
    !pte_write(entry) = true  (AP[2]=1 in PTE, read-only)
    → do_wp_page(vmf)   // Write-Protect page = Copy-On-Write!

do_wp_page():
  1. wp_page_shared(): if page->_mapcount > 0 (shared between processes):
       // Must copy since other process still maps original
       alloc_page(GFP_HIGHUSER_MOVABLE)
       copy_user_highpage(new_page, old_page, addr, vma)
       // ARM64: memcpy 4KB, uses NEON/SVE if available
       page_remove_rmap(old_page, vma)
       set_pte_at(): install new PTE pointing to new_page, writable
       page_add_anon_rmap(new_page, vma, addr)
       put_page(old_page)
       return VM_FAULT_MINOR  // CoW handled

  2. wp_page_reuse(): if mapcount == 0 (only this process maps it):
       // No need to copy! Just make it writable
       entry = pte_mkwrite(old_entry)
       set_pte_at(): update PTE in place (AP[2]=0 → writable)
       flush_tlb_page(vma, addr)
       return VM_FAULT_NOPAGE
  
  3. wp_page_copy(): if shared but can't reuse:
       → full copy path (same as shared above)

After CoW:
  Parent and child each have INDEPENDENT physical pages
  Parent's page: AP[2]=1 (still read-only for other potential sharers)
  Child's page: AP[2]=0 (writable) if no other sharer, or new copy

fork() interaction:
  copy_page_range(): for all writable PTEs in parent:
    → set parent PTE to read-only (AP[2]=1)
    → set child PTE to read-only (AP[2]=1)
    → both share physical page
  First write by either: protection fault → do_wp_page() → CoW
```

---

## 6. Interview Questions & Answers

**Q1: A process calls fork(). Both parent and child then write to the same heap variable. How many page faults occur and what type are they?**

After `fork()`, `copy_page_range()` marks all writable heap pages **read-only** in BOTH parent and child PTEs (but they still share the same physical page). The `anon_vma` is set up to track this sharing.

**Fault 1 (first writer)**: Suppose the child writes first. It gets a **protection fault** (permission fault on ARM64, ESR DFSC = 0b001111). `do_wp_page()` is called. Since `page->_mapcount > 0` (parent also maps it), the kernel allocates a new physical page, copies the content, and installs a writable PTE in the child's page table. The parent's PTE is updated to remain read-only (if it still needs to be protected) or may be made writable if the child's fork has resolved all sharing. This is a **minor fault** (VM_FAULT_MINOR, no disk I/O).

**Fault 2 (second writer)**: Now the parent writes. It also gets a **protection fault**. `do_wp_page()` is called again. This time, `page->_mapcount == 0` (only the parent maps the original page now). So `wp_page_reuse()` simply sets `AP[2]=0` in the parent's PTE (makes it writable in-place, no copy). TLB flush for that page. Also a **minor fault**.

**Total: 2 page faults, both minor (protection/CoW type).** No disk I/O occurs. This is why `fork()` is efficient: pages are only copied when actually written, not all at once.

---

## 7. Quick Reference

| Fault Type | Cause | Handler | VM_FAULT_MAJOR? |
|---|---|---|---|
| Minor (demand anon) | First access to anonymous page | do_anonymous_page | No |
| Minor (file in cache) | File page in page cache, not in PTE | do_read_fault + filemap_fault | No |
| Major (swap) | Page swapped out to disk | do_swap_page + swap_readpage | Yes |
| Major (file) | File page not in cache, need read | do_read_fault + submit_bio | Yes |
| Protection (CoW) | Write to read-only shared page | do_wp_page | No |
| SIGSEGV | No VMA, wrong permissions | bad_area → force_sig | N/A |
| SIGBUS | IO error, MAP_FIXED beyond file end | __do_user_fault SIGBUS | N/A |

| ARM64 ESR.DFSC | Fault | Resolution |
|---|---|---|
| 0b000011 (L3 translation) | PTE missing | Install PTE (demand page or swap-in) |
| 0b001111 (L3 permission) | Write to RO PTE | CoW (do_wp_page) |
| 0b001011 (L3 AF) | Access flag not set | Set AF bit (pte_mkyoung) |
| N/A (no VMA) | Invalid address | SIGSEGV |
