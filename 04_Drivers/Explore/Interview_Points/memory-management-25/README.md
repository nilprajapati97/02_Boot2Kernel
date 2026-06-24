# Linux Memory Management — Senior Engineer Deep Dive (25 Questions)

A focused, in-depth study pack of **25 Memory Management deep-dive documents** for senior
(9+ years) Linux kernel engineer interviews at **Google, NVIDIA, Qualcomm, and AMD**.

Each document is engineered so you can **talk for 30+ minutes** on the topic — covering
**Why / When / Where / How**, data-structure diagrams, annotated kernel C, company-specific
angles, a war story, and likely interviewer follow-ups.

> **Kernel baseline:** v6.6+ (folios, maple-tree VMAs, MGLRU, memcg v2, per-VMA locks,
> memory tiering / CXL, DAMON). Legacy behavior is called out where it still comes up.

> **Companion set:** general MM fundamentals (page-table walk, buddy/SLUB, kmalloc vs
> vmalloc, page reclaim/OOM, rmap) live in [../01-memory-management/](../01-memory-management).
> This set goes **deeper and wider** with minimal overlap; cross-links are provided.

---

## How to use this pack

1. **First pass (recall):** read only the **TL;DR cheat sheet** at the top of each doc.
2. **Second pass (depth):** read the full **Why / When / Where / How** + diagrams.
3. **Third pass (performance):** say the **30-Min Talk Track** out loud; have someone fire the
   **Interviewer Follow-ups** at you.
4. **Night before:** re-read all 25 TL;DRs + every **War Story** — war stories separate a
   9-year engineer from a textbook answer.

### Legend (every document uses these sections)

| Section | Purpose |
|---------|---------|
| **TL;DR Cheat Sheet** | 30-second rapid recall: key structs, APIs, one-liners |
| **Why** | The problem it solves, design rationale, trade-offs |
| **When** | Triggers / when to use vs alternatives |
| **Where** | Source files, structs, subsystem boundaries |
| **How** | The deep step-by-step mechanics — your core narrative |
| **Diagrams** | ASCII data-structure layouts + Mermaid flow/sequence |
| **Annotated C** | Real kernel structs/APIs with commentary |
| **Company Angle** | Why each target company probes this |
| **War Story** | A concrete root-cause narrative you can tell |
| **Interviewer Follow-ups** | Likely follow-ups with crisp answers |
| **30-Min Talk Track** | Minute-by-minute speaking outline |

---

## Index

### 1. Virtual Memory
| # | Topic | Doc |
|---|-------|-----|
| 1 | Process address space & ASLR (maple-tree VMAs) | [01-process-address-space-aslr.md](01-virtual-memory/01-process-address-space-aslr.md) |
| 2 | `struct page`, folios & memdescs | [02-struct-page-and-folios.md](01-virtual-memory/02-struct-page-and-folios.md) |
| 3 | Page fault handling end-to-end | [03-page-fault-handling.md](01-virtual-memory/03-page-fault-handling.md) |
| 4 | Copy-on-write (fork, GUP-vs-CoW) | [04-copy-on-write.md](01-virtual-memory/04-copy-on-write.md) |
| 5 | Demand paging & overcommit | [05-demand-paging-overcommit.md](01-virtual-memory/05-demand-paging-overcommit.md) |

### 2. Physical Allocators
| # | Topic | Doc |
|---|-------|-----|
| 6 | Memory models: FLATMEM vs SPARSEMEM, hotplug | [06-memory-models-sparsemem.md](02-physical-allocators/06-memory-models-sparsemem.md) |
| 7 | Zones, watermarks, zonelist fallback | [07-zones-watermarks-fallback.md](02-physical-allocators/07-zones-watermarks-fallback.md) |
| 8 | Per-CPU page allocator & GFP fast path | [08-per-cpu-page-allocator.md](02-physical-allocators/08-per-cpu-page-allocator.md) |
| 9 | Compaction & external fragmentation | [09-compaction-fragmentation.md](02-physical-allocators/09-compaction-fragmentation.md) |
| 10 | CMA — contiguous memory allocator | [10-cma-contiguous-allocator.md](02-physical-allocators/10-cma-contiguous-allocator.md) |

### 3. Page Cache & Writeback
| # | Topic | Doc |
|---|-------|-----|
| 11 | Page cache: address_space, xarray, readahead | [11-page-cache-xarray-readahead.md](03-page-cache-writeback/11-page-cache-xarray-readahead.md) |
| 12 | Dirty pages & writeback | [12-dirty-writeback.md](03-page-cache-writeback/12-dirty-writeback.md) |
| 13 | mmap vs read/write; SHARED vs PRIVATE | [13-mmap-vs-rw-shared-private.md](03-page-cache-writeback/13-mmap-vs-rw-shared-private.md) |

### 4. Reclaim & Swap
| # | Topic | Doc |
|---|-------|-----|
| 14 | Swap subsystem (swap cache, zswap/zram) | [14-swap-internals.md](04-reclaim-swap/14-swap-internals.md) |
| 15 | MGLRU (multi-gen LRU) deep dive | [15-mglru.md](04-reclaim-swap/15-mglru.md) |
| 16 | PSI & proactive reclaim | [16-psi-proactive-reclaim.md](04-reclaim-swap/16-psi-proactive-reclaim.md) |
| 17 | kswapd vs direct reclaim vs madvise | [17-kswapd-direct-reclaim-madvise.md](04-reclaim-swap/17-kswapd-direct-reclaim-madvise.md) |

### 5. Hugepages & TLB
| # | Topic | Doc |
|---|-------|-----|
| 18 | Hugetlbfs vs THP | [18-hugetlb-vs-thp.md](05-hugepages-tlb/18-hugetlb-vs-thp.md) |
| 19 | TLB shootdown & mmu_gather | [19-tlb-shootdown-mmu-gather.md](05-hugepages-tlb/19-tlb-shootdown-mmu-gather.md) |

### 6. NUMA & Memory Tiering
| # | Topic | Doc |
|---|-------|-----|
| 20 | NUMA policy & AutoNUMA | [20-numa-policy-autonuma.md](06-numa-tiering/20-numa-policy-autonuma.md) |
| 21 | NUMA allocation & memory tiering / CXL | [21-numa-allocation-memory-tiering.md](06-numa-tiering/21-numa-allocation-memory-tiering.md) |

### 7. Cgroups & Accounting
| # | Topic | Doc |
|---|-------|-----|
| 22 | Memory cgroup v2 (memcg) | [22-memcg-v2.md](07-cgroups-accounting/22-memcg-v2.md) |

### 8. Advanced Page Tables
| # | Topic | Doc |
|---|-------|-----|
| 23 | Page-table lifecycle & mmu_notifier (HMM/SVA) | [23-mmu-notifier-and-pagetables.md](08-advanced-pgtables/23-mmu-notifier-and-pagetables.md) |
| 24 | KSM — kernel same-page merging | [24-ksm.md](08-advanced-pgtables/24-ksm.md) |

### 9. MM Debugging
| # | Topic | Doc |
|---|-------|-----|
| 25 | MM debugging: KASAN/KFENCE/page_owner/DAMON | [25-mm-debugging-kasan-kfence.md](09-mm-debugging/25-mm-debugging-kasan-kfence.md) |

---

## Company emphasis matrix

| Company | Memory-management focus | Must-nail docs |
|---------|-------------------------|----------------|
| **Google** | memcg v2, PSI/proactive reclaim, MGLRU, THP at scale, cgroup OOM, DAMON | 15, 16, 22, 25 |
| **NVIDIA** | mmu_notifier/HMM, GUP pinning vs CoW, TLB shootdown, hugepages, CMA | 4, 18, 19, 23 |
| **Qualcomm** | zones/CMA, zram/zswap, low-RAM reclaim, PSI/lmkd, ARM TLBI, overcommit | 5, 10, 14, 19 |
| **AMD** | NUMA policy/AutoNUMA, memory tiering/CXL, compaction, watermarks, scaling | 7, 9, 20, 21 |

---

## Senior-level reminders

- Interviewers want **war stories** — concrete bugs you root-caused (THP latency spikes,
  GUP-vs-CoW data corruption, memcg OOM storms, CMA allocation failures), patches you
  upstreamed, and **trade-off** discussions.
- Be ready to **whiteboard data structures**: `struct folio`, `vm_area_struct`, the maple
  tree, `struct mem_cgroup`, MGLRU generations, the xarray page cache.
- Always reason from **hardware first**: TLB reach, cache lines, NUMA distance, page size.
- State trade-offs explicitly ("I'd use X because Y, accepting cost Z").
