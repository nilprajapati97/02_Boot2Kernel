# THP and khugepaged Deep Dive

**Category**: Huge Pages and Page Size Optimization  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
THP (Transparent Huge Pages) — key design goals:
  1. No application change required (vs hugetlbfs)
  2. Automatic: kernel decides when to use 2MB pages
  3. Fallback: silently falls back to 4KB if 2MB unavailable
  4. Works for: anonymous memory (heap, stack, mmap private)
  5. Also: file-backed THP (Linux 6.x+ for tmpfs, ext4 with LARGE_FOLIO)

Two THP allocation paths:
  A) Fault-time THP: allocate 2MB huge page on first access (eager)
  B) khugepaged: background collapse of existing 4KB pages into 2MB

ARM64 2MB huge page in page tables:
  PMD entry (L2 page table):
    bits[1:0] = 0b01 (block entry, NOT 0b11 which is table entry)
    bits[47:21] = PA[47:21] (2MB-aligned physical address)
    bits[63:52] = attributes (access perms, memory type, sharability)
  
  This replaces the entire L3 page table (512 × 4KB PTEs = 4KB of PTEs)
  Saving: 4KB of page table memory + 512 TLB entries → 1 TLB entry
```

---

## 2. THP Fault-Time Allocation

```
do_anonymous_page() (arch/arm64 + mm/memory.c):

Normal 4KB fault path:
  fault → do_page_fault → __handle_mm_fault → handle_pte_fault
  → do_anonymous_page:
    alloc_pages(GFP_HIGHUSER_MOVABLE, 0): order=0 (4KB)
    set_pte_at(): install 4KB PTE

THP fault path (2MB):
  fault → do_page_fault → __handle_mm_fault
  → Before handling PTE: check for THP at PMD level
  
  create_huge_pmd():
    // Check conditions for THP allocation:
    1. Is request PMD-aligned? (address PMD_MASK = ~(2MB-1))
    2. Is THP enabled? (/sys/kernel/mm/transparent_hugepage/enabled != never)
    3. Is MADV_NOHUGEPAGE set? If yes: skip THP
    4. Is VMA eligible? (anonymous, not mlocked, not special)
    5. Try alloc_pages(GFP_TRANSHUGE, HPAGE_PMD_ORDER):
       GFP_TRANSHUGE = GFP_HIGHUSER_MOVABLE | __GFP_COMP | __GFP_NOMEMALLOC
       HPAGE_PMD_ORDER = 9 (2^9 = 512 × 4KB pages = 2MB)
    
    If allocation succeeds:
      prep_transhuge_page(): initialize compound page
      pmd_mk_huge(): make PMD entry as block descriptor
                     ARM64: set bits[1:0]=0b01 in PMD
      set_pmd_at(): install 2MB block entry in PMD
      count_vm_event(THP_FAULT_ALLOC): increment counter
      return: 2MB mapped, fault handled
    
    If allocation FAILS (memory fragmented, pressure high):
      fall back to:
      pte_alloc(): create L3 table
      alloc_pages(order=0): 4KB page
      set_pte_at(): install 4KB PTE
      // Silently fell back — application never knows
      count_vm_event(THP_FAULT_FALLBACK): increment counter

Checking THP fault stats:
  /proc/vmstat:
    thp_fault_alloc: successful 2MB fault allocations
    thp_fault_fallback: fell back to 4KB
    thp_fault_fallback_charge: fell back due to cgroup memory limit
  
  thp_fault_alloc / (thp_fault_alloc + thp_fault_fallback) = THP hit rate
```

---

## 3. khugepaged — Background Huge Page Collapse

```
khugepaged: kthread that proactively collapses 4KB pages into 2MB THP

Why needed: many allocations start as 4KB pages (before THP was available
or before alignment conditions were met). khugepaged retrofits huge pages.

khugepaged activation:
  Enabled automatically when THP is not "never"
  Runs as: [khugepaged] kthread (one per system, not per-CPU)
  Sleep: wakes every khugepaged_scan_sleep_millisecs (default: 10ms)
  Work budget: scans up to khugepaged_pages_to_scan pages per wake

khugepaged_scan_mm_slot() → khugepaged_scan_pmd():

For each process in the mm_slot list:
  For each VMA in the process:
    Find: PMD-aligned 2MB regions (address % PMD_SIZE == 0)
    
    Check collapse prerequisites:
      1. All 512 pages must be PRESENT (resident, not swapped)
      2. All 512 pages must be ANONYMOUS (no file-backed pages)
      3. None of 512 pages are mapped shared (COW resolved)
      4. None are mlocked, pinned, or have elevated refcount
      5. Node has enough free 2MB-order memory (order-9 free page)
      6. No page migration in progress on these pages
    
    If all conditions met:
      khugepaged_collapse_huge_page():
        1. alloc_pages(GFP_TRANSHUGE, 9): allocate new 2MB page
        2. For each of 512 pages:
             copy_highpage(newpage + i*PAGE_SIZE, oldpage[i])
        3. __collapse_huge_page_isolate():
             Remove all 512 pages from LRU
             Set pte_none() for each (clear all 512 PTEs)
             Flush TLB: tlb_flush_range() for entire 2MB
        4. Set PMD block entry: pmd_mkhuge() + set_pmd_at()
        5. add_new_anon_rmap(): register new 2MB compound page
        6. Free the 512 individual pages (put_page() × 512)
        7. count_vm_event(THP_COLLAPSE_ALLOC)
      
    If conditions NOT met (any page fails check):
      Skip this 2MB region, move to next

khugepaged memory addition hook:
  khugepaged_enter_vma():
    Called when new memory is mmap'd (mmap, brk extension)
    Adds VMA to khugepaged's scan list
    khugepaged wakes up sooner to attempt collapse

khugepaged stats:
  /proc/vmstat:
    thp_collapse_alloc: successful khugepaged collapses
    thp_collapse_alloc_failed: khugepaged failed to get 2MB page
    thp_zero_page_alloc: zero-page THP allocated
    thp_split_page: 2MB THP split back to 4KB
    thp_split_pmd: PMD split (THP stays but PMD→L3 table)
    thp_scan_exceed_none_pte: scan stopped early (too many holes)
```

---

## 4. THP Split

```
THP split: converting 2MB page back to 512 × 4KB pages
  When needed:
    - COW: fork → child writes to THP → split needed (if not enough 2MB memory)
    - msync() / mprotect(): partial region change needs fine-grained control
    - madvise(MADV_DONTNEED): partial range invalidation
    - swap pressure: swap out part of THP (need 4KB granularity)
    - Memory hotplug: offline individual pages from THP range
    
split_huge_pmd(vma, pmd, address):
  Called when: need to convert 2MB PMD block → L3 table + 512 PTEs
  
  Steps:
    1. alloc_page(GFP_KERNEL): allocate new L3 table page
    2. For each sub-page i (0..511):
         mk_pte(compound_page + i, vma->vm_page_prot): create PTE
         set_pte_at(new_pte_table + i): install 4KB PTE
    3. pmd_populate(mm, pmd, new_pte_table): install L3 table in PMD
    4. TLB flush: flush the 2MB PMD entry (TLBI VAE1IS or ASIDE1IS)
    5. Count: count_vm_event(THP_SPLIT_PMD)
  
  Note: this splits the PMD entry only (512 PTEs all point to same compound page)
  The compound page itself is NOT split yet → 512 PTEs share the 2MB compound page
  
split_huge_page(page):
  Splits the underlying compound page into 512 independent pages
  Called when: individual pages need separate lifecycle (swap, migration)
  
  Steps:
    1. For each sub-page: prep_compound_page(tail, 0) to make independent
    2. Update: refcounts, mapcount, LRU for each sub-page
    3. compound_head(page): no longer compound → 512 independent struct pages

THP and COW:
  Fork (copy_page_range()):
    For each PMD:
      If huge (block): 2MB COW
      copy_huge_pmd(): mark both parent + child PMD as read-only (soft dirty)
      On first write: 
        do_huge_pmd_wp_page():
          if (reuse possible): just clear write-protect (no copy)
          else: alloc 2MB for child + copy → expensive!
  
  Optimization (Linux 5.13+):
    If only ONE reference to THP: reuse (no copy)
    If multiple references: split to 4KB, copy only the dirty page
    Avoids: copying 2MB when only 1 page was actually written
```

---

## 5. THP for File-Backed Memory (Linux 6.x+)

```
THP for file mappings (large folio support):
  Historically: THP only for anonymous memory
  Linux 6.1+: THP for tmpfs, 6.x ongoing: ext4, xfs, btrfs

Large folio in page cache:
  struct folio: represents multiple pages (2^N) in page cache
  For tmpfs with large folio enabled:
    readahead: allocate folio of order 9 (2MB) for large files
    folio mapped: PMD block entry (2MB) in page table
    Write: entire 2MB folio dirtied as one unit
    Writeback: 2MB written to swap/tmpfs in one bio
  
  Benefits: fewer page table entries for large mmap'd files
            fewer writeback operations (coarser dirty tracking)
            same TLB efficiency as anonymous THP

THP for shmem (tmpfs):
  CONFIG_TRANSPARENT_HUGEPAGE + tmpfs:
    Mount option: huge=always/within_size/advise/never
    within_size: THP only if file content fits entirely in 2MB granule
    
  Checking:
    /sys/kernel/mm/transparent_hugepage/shmem_enabled
```

---

## 6. Interview Q&A

**Q1: What are the conditions khugepaged checks before collapsing a region?**
khugepaged checks (all must pass): (1) All 512 pages are PRESENT — no swap entries, no holes. (2) All pages are ANONYMOUS — no file-backed pages (even partially). (3) No shared pages (all COW must be resolved). (4) No mlocked pages. (5) No pages with elevated refcount (e.g., pinned for DMA). (6) A free order-9 compound page (2MB contiguous) is available on the target NUMA node. (7) No memory migration pending. If any check fails: skip this 2MB region. The strictness ensures no data corruption when replacing 512 PTEs with one PMD.

**Q2: What is the performance impact of THP on fork()?**
Fork with THP can be expensive for write-heavy workloads. When child writes to a shared 2MB THP page: COW triggers → kernel must allocate a new 2MB compound page and copy 2MB of data. Compare to 4KB: COW copies only 4KB. For a process that forks and writes to most of its memory: 512× more data copied per modified range. Linux optimization (5.13+): if only the written 4KB within the 2MB was modified: split the THP to 4KB pages and only copy the single dirty 4KB. This reduces the COW penalty dramatically for sparse-write workloads.

**Q3: How does splitting a THP work at the page table level?**
`split_huge_pmd()` converts the 2MB block PMD entry into a regular L3 table. Steps: (1) Allocate one 4KB page for the new L3 table. (2) For each of 512 sub-pages: create a PTE pointing to (compound_base + i×PAGE_SIZE). (3) Install the L3 table address in the PMD (set PMD bits[1:0]=0b11 for table, not block). (4) TLB flush the 2MB range (one `TLBI VAE1IS` for the PMD, or 512 individual PTEs — Linux broadcasts for the PMD entry). After split: 512 PTEs exist, all pointing to sub-pages of the original compound page. The compound page itself may remain compound until `split_huge_page()` is called.

**Q4: What is THP_FAULT_FALLBACK and when does it happen?**
`THP_FAULT_FALLBACK` is incremented when `do_anonymous_page()` tried to allocate a 2MB page (order=9) but failed. Failure reasons: (1) No contiguous 2MB-aligned free range in the buddy allocator (memory fragmentation). (2) Memory pressure: `GFP_TRANSHUGE` includes `__GFP_NOMEMALLOC` — doesn't use emergency reserves. (3) cgroup memory limit would be exceeded. On fallback: the kernel silently allocates a 4KB page instead. High `thp_fault_fallback` rate indicates: memory fragmentation → consider memory compaction, or tune THP defrag policy to `defer` or `madvise` to avoid compaction latency.

**Q5: What is the difference between thp_split_pmd and thp_split_page?**
`thp_split_pmd`: splits the PMD entry from a 2MB block descriptor to an L3 table with 512 PTEs. The underlying compound page REMAINS intact (512 sub-page PTEs all point to the same compound page). `thp_split_page`: splits the underlying compound page itself into 512 independent `struct page` objects. Each gets its own `_refcount`, LRU state, etc. This is needed for: swapping out individual sub-pages, memory hotplug page offlining, DMA pinning of individual sub-pages. A THP split is complete (full independence) only after `thp_split_page`.

---

## 7. Quick Reference

| THP Path | Trigger | ARM64 Action |
|---|---|---|
| Fault-time | First fault on 2MB-aligned anonymous VMA | PMD block entry (order-9 alloc) |
| khugepaged | 512 × 4KB pages in same 2MB range | Collapse: copy → PMD block |
| Split PMD | mprotect/partial MADV_DONTNEED/COW | PMD block → L3 table + 512 PTEs |
| Split page | Swap, migration, DMA pin | Compound page → 512 independent |

| sysctl / sys | Default | Effect |
|---|---|---|
| `transparent_hugepage/enabled` | `madvise` | THP allocation policy |
| `transparent_hugepage/defrag` | `defer` | khugepaged + compaction policy |
| `khugepaged/pages_to_scan` | 4096 | Pages scanned per khugepaged run |
| `khugepaged/scan_sleep_millisecs` | 10 | khugepaged sleep between runs |
