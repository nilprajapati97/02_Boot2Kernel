# Q02 — Design a Virtual Memory Management System (Paging, Swapping, NUMA Awareness)

---

## 1. Problem Statement

Design a virtual memory management (VMM) subsystem from scratch that handles:
- Page table management (multi-level, architecture-agnostic)
- Demand paging with transparent huge page (THP) support
- Page reclaim and swap under memory pressure
- NUMA-aware page placement and migration

This is the core of the Linux VMM — `mm/` directory. A 10-year kernel engineer must understand why Linux made every design decision in this space and what alternatives were rejected.

---

## 2. Requirements

### 2.1 Functional Requirements
- Virtual address space isolation per process (user + kernel split).
- Demand paging: allocate physical pages lazily on first access.
- Copy-on-write (COW) for `fork()` semantics.
- Swap to persistent storage under memory pressure (anonymous pages).
- File-backed mappings via page cache integration.
- Huge page support: 2MB and 1GB pages (x86-64).
- NUMA: prefer local-node allocation; migrate cold pages to remote nodes.
- `mmap()`, `brk()`, `mremap()` syscall implementation.

### 2.2 Non-Functional Requirements
- TLB shootdown latency < 5 µs on 128-core systems.
- Page fault handling time < 1 µs for hot paths (minor fault).
- Memory allocator overhead < 0.5% of system RAM for metadata.
- Lock contention on page tables must not scale with number of threads.

---

## 3. Constraints & Assumptions

- 64-bit architecture (x86-64 canonical address space: 128 TB user, 128 TB kernel).
- 4-level page tables (PGD → P4D → PUD → PMD → PTE) — can extend to 5-level (LA57).
- Physical memory up to 16 TB (48-bit physical address space).
- NUMA topology: up to 8 nodes, each up to 2 TB RAM.
- Kernel version: Linux 6.x (`folio`-based page management, `struct folio` replaces `struct page` for compound pages).

---

## 4. Architecture Overview

```
Virtual Address (64-bit)
  ┌──────────────────────────────────────────────────────────┐
  │ [63:48] sign ext │ [47:39] PGD │ [38:30] PUD │ [29:21] PMD │ [20:12] PTE │ [11:0] offset │
  └──────────────────────────────────────────────────────────┘
         │
         ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │                    Page Table Walk                              │
  │                                                                 │
  │  mm->pgd ──► PGD[idx] ──► PUD[idx] ──► PMD[idx] ──► PTE[idx]  │
  │                                                    │           │
  │                                                    ▼           │
  │                                              pfn → struct folio │
  └─────────────────────────────────────────────────────────────────┘
         │
         ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │                    Page Fault Handler                          │
  │                                                                 │
  │   do_page_fault()                                              │
  │       ├── do_anonymous_page()   [anon private, stack]          │
  │       ├── do_cow_page()         [COW on write fault]           │
  │       ├── do_swap_page()        [swapped-out page]             │
  │       └── do_fault()            [file-backed via VMA ops]      │
  └─────────────────────────────────────────────────────────────────┘
         │
         ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │              Memory Reclaim (kswapd / direct reclaim)          │
  │                                                                 │
  │   LRU lists: active_anon, inactive_anon, active_file,          │
  │              inactive_file, unevictable                        │
  │                                                                 │
  │   Reclaim order: inactive_file → inactive_anon → swap           │
  └─────────────────────────────────────────────────────────────────┘
         │
         ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │             NUMA Policy Layer                                  │
  │  mempolicy: MPOL_DEFAULT, MPOL_BIND, MPOL_PREFERRED, MPOL_INTERLEAVE │
  │  Automatic NUMA balancing: periodic unmapping + fault-based migration │
  └─────────────────────────────────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Memory Descriptor — `struct mm_struct`
```c
struct mm_struct {
    struct maple_tree   mm_mt;          /* VMA tree (replaces rb_tree in 6.1+) */
    pgd_t              *pgd;           /* top-level page table */
    atomic_t            mm_users;      /* # threads sharing this mm */
    atomic_t            mm_count;      /* reference count */
    unsigned long       mmap_base;     /* base of mmap region */
    unsigned long       start_brk, brk; /* heap bounds */
    unsigned long       start_stack;   /* stack top */
    struct mmu_notifier_subscriptions *notifier_subscriptions;
    /* NUMA statistics, rss counters, etc. */
    spinlock_t          page_table_lock; /* protects PTE modifications */
    struct rw_semaphore mmap_lock;      /* serializes VMA modifications */
};
```

### 5.2 Virtual Memory Area — `struct vm_area_struct`
```c
struct vm_area_struct {
    unsigned long   vm_start, vm_end;  /* [start, end) virtual range */
    unsigned long   vm_flags;          /* VM_READ, VM_WRITE, VM_EXEC, VM_SHARED */
    struct mm_struct *vm_mm;
    pgprot_t        vm_page_prot;      /* PTEs protection bits */
    const struct vm_operations_struct *vm_ops; /* fault, open, close */
    /* file-backed */
    struct file    *vm_file;
    unsigned long   vm_pgoff;          /* offset into file (in pages) */
    /* anon */
    struct anon_vma *anon_vma;         /* for RMAP reverse mapping */
};
```

### 5.3 Physical Page — `struct folio` (Linux 5.16+, replaces struct page for compound)
```c
struct folio {
    /* first page */
    unsigned long flags;               /* PG_locked, PG_uptodate, PG_dirty, PG_lru */
    struct list_head lru;              /* LRU list linkage */
    struct address_space *mapping;     /* page cache or anon_vma */
    pgoff_t         index;             /* offset in mapping */
    atomic_t        _mapcount;         /* # PTEs mapping this folio */
    atomic_t        _refcount;         /* reference count */
    /* for compound/huge pages: order encodes size */
    unsigned int    order;             /* 0=4K, 9=2MB, 18=1GB */
};
```

### 5.4 Reverse Mapping — `struct anon_vma` + `struct anon_vma_chain`
```c
/* Allows finding all PTEs mapping a given page — needed for reclaim/migration */
struct anon_vma {
    struct anon_vma    *root;
    struct rw_semaphore rwsem;
    atomic_t            refcount;
    struct rb_root_cached rb_root;  /* tree of anon_vma_chain */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Page Table Walk with Huge Page Shortcut
```
pgd = pgd_offset(mm, addr)
p4d = p4d_offset(pgd, addr)
pud = pud_offset(p4d, addr)

if pud_huge(*pud):
    return pud_page_vaddr(*pud) + (addr & ~PUD_MASK)  // 1GB page shortcut

pmd = pmd_offset(pud, addr)

if pmd_huge(*pmd):
    return pmd_page_vaddr(*pmd) + (addr & ~PMD_MASK)  // 2MB page shortcut

pte = pte_offset_map_lock(mm, pmd, addr, &ptl)
```

### 6.2 Anonymous Page Fault (do_anonymous_page)
1. Check VMA permissions.
2. `alloc_folio_vma()` → NUMA-local page from zone allocator.
3. Zero the page (security — prevent info leak from freed pages).
4. Install PTE: `mk_pte(folio_page(folio, 0), vma->vm_page_prot)`.
5. Add folio to LRU via `folio_add_lru()`.
6. Increment RSS counter for process.

### 6.3 Copy-on-Write (COW)
```
On write fault to read-only PTE:
  if (page_count(page) == 1 && !page_maybe_dma_pinned(page))
      // exclusive — just make writable (fast path, no copy)
      set_pte(pte, pte_mkwrite(pte))
  else
      // shared — must copy
      new_folio = alloc_folio_vma(GFP_KERNEL, vma, addr)
      copy_user_highpage(new_folio, old_folio)
      set_pte(pte, mk_pte(new_folio_page))
      page_remove_rmap(old_folio)
      folio_put(old_folio)
```

### 6.4 Page Reclaim — LRU Clock Algorithm
Linux uses a **two-list LRU** (active + inactive) per zone per LRU type:
- Pages start on `inactive` list.
- `mark_page_accessed()` promotes to `active` list (second-chance).
- Reclaim scans `inactive` list first.
- `shrink_inactive_list()` → tries to unmap PTEs via RMAP → writeback dirty pages → free.

**Why two-list over single LRU:** prevents sequential scan thrashing from evicting hot working set.

### 6.5 Swap Path
```
Reclaim decision: writeback or swap
  File-backed page → writeback to page cache backing store
  Anonymous page   → swap_writepage() → submit bio to swap device
                  → pte_to_swp_entry(entry) stored in PTE slot
                     (PTE present bit = 0, rest = swap entry)

Swap-in path (do_swap_page):
  entry = pte_to_swp_entry(*pte)
  folio = swapin_readahead(entry, ...)  // reads swap cluster
  set_pte(pte, mk_pte(folio_page))
  swap_free(entry)
```

### 6.6 TLB Shootdown (mmu_gather)
```c
tlb_gather_mmu(&tlb, mm);
    for each PTE to unmap:
        pte = ptep_get_and_clear(mm, addr, ptep)
        tlb_remove_page(&tlb, pfn_to_page(pte_pfn(pte)))
tlb_finish_mmu(&tlb);
    // batches TLB invalidations, then sends IPI to all CPUs running this mm
    // uses mmu_notifier for KVM/IOMMU guests
```

### 6.7 NUMA Balancing
Linux `NUMA_BALANCING` periodically:
1. Unmaps PTEs (marks them as `pte_numa()` — valid but triggers fault).
2. On next access fault: checks if accessing CPU is local to page's node.
3. If remote: `migrate_pages()` moves folio to local node.
4. Updates `task_struct::numa_preferred_nid` based on fault statistics.

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| VMA storage | Maple tree (6.1+) | Red-black tree | Maple tree has better cache behavior for range lookups |
| Page metadata | `struct folio` | `struct page` array | Folio unifies compound page metadata, reduces indirection |
| Reclaim policy | Two-list LRU (active/inactive) | Single clock LRU | Resists thrashing from streaming I/O |
| TLB batching | `mmu_gather` lazy flush | Immediate IPI per unmap | Batching amortizes IPI cost on bulk unmaps |
| NUMA migration | Soft — fault-based | Hard — periodic scan | Fault-based has lower overhead; tracks actual access pattern |
| Huge pages | THP — transparent, automatic | Explicit `mmap(MAP_HUGETLB)` | THP works without app changes; fallback to 4K on failure |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol / Function |
|---|---|---|
| Page fault entry | `arch/x86/mm/fault.c` | `do_page_fault()`, `handle_mm_fault()` |
| Anonymous fault | `mm/memory.c` | `do_anonymous_page()`, `do_cow_fault()` |
| Page reclaim | `mm/vmscan.c` | `shrink_inactive_list()`, `kswapd()` |
| LRU management | `mm/swap.c` | `folio_add_lru()`, `mark_page_accessed()` |
| Swap | `mm/swap_state.c`, `mm/swapfile.c` | `swap_writepage()`, `do_swap_page()` |
| TLB gather | `mm/mmu_gather.c` | `tlb_gather_mmu()`, `tlb_finish_mmu()` |
| NUMA balancing | `mm/numa.c` | `do_numa_page()`, `task_numa_fault()` |
| Folio API | `include/linux/mm_types.h` | `struct folio`, `folio_alloc()` |
| VMA maple tree | `mm/mmap.c` | `vma_find()`, `vma_iter_next_range()` |
| RMAP | `mm/rmap.c` | `page_referenced()`, `try_to_unmap()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Page Table Corruption
**Symptom:** Random SIGSEGV, `BUG: unable to handle kernel paging request`.
**Debug:**
```bash
echo 1 > /proc/sys/kernel/panic_on_oops   # capture full oops
# In oops output: CR2 = faulting VA, RIP = instruction
# Use addr2line or gdb vmlinux to decode
objdump -d vmlinux | grep -A5 "<do_page_fault>"
```

### 9.2 Memory Leak (anon pages not freed)
**Symptom:** RSS grows unbounded; `kswapd` thrashing.
**Debug:**
```bash
cat /proc/<pid>/smaps | grep -A10 "Anonymous"
# Check for large VMA with high Anonymous: and low Shared_Dirty:
# Use kmemleak: echo scan > /sys/kernel/debug/kmemleak
```

### 9.3 TLB Shootdown Latency Spike
**Symptom:** `munmap()` takes milliseconds on large mappings.
**Debug:**
```bash
perf trace -e tlb:tlb_flush -- your_workload
# Count IPIs during shootdown
cat /proc/interrupts | grep TLB
```

### 9.4 NUMA Migration Thrashing
**Symptom:** `numastat` shows high `numa_miss`; performance degrades.
**Fix:** Pin threads to NUMA node or set `MPOL_BIND`:
```bash
numactl --membind=0 --cpunodebind=0 ./workload
```

---

## 10. Performance Considerations

- **Huge pages for GPU buffers:** GPU DMA regions should use 2MB THPs to reduce TLB pressure. Map with `MAP_HUGETLB` or rely on `khugepaged` to collapse.
- **Page table lock granularity:** `page_table_lock` is per-`mm_struct`. For large multi-threaded processes, PTE-level locking via `pte_lockptr()` (per-PMD ptlock) reduces contention.
- **NUMA-first allocation:** `alloc_pages_node(nid, ...)` in page fault path ensures pages land on local node.
- **Readahead for swap:** `swapin_readahead()` reads a cluster of swap pages in one I/O — amortizes latency for sequential access patterns.
- **Zero-page dedup:** All-zero anonymous pages initially map to `ZERO_PAGE(0)` — a shared read-only page. COW breaks this on first write. Saves RAM for sparse allocations.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. 5-level page table awareness (LA57 on Sapphire Rapids / Genoa).
2. `struct folio` transition — why it was needed (compound page confusion in old API).
3. You can trace a page fault from `do_page_fault()` through `handle_mm_fault()` → VMA ops → zone allocator → LRU.
4. Why Linux uses two-list LRU, not a simple clock — file cache thrashing resistance.
5. `mmu_notifier` for KVM/VFIO/GPU IOMMU integration — critical for NVIDIA's UVM driver.
6. RMAP (reverse map) and why it's needed for reclaim — given a `struct page`, find all PTEs pointing to it.
7. NUMA: `mbind()` / `set_mempolicy()` system calls, `MPOL_BIND` for GPU memory pinning.
