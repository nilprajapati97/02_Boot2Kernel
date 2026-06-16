# Linux Kernel MM: Complete Interview Reference

**Category**: Linux Kernel Memory Management — Synthesis  
**Platform**: ARM64 (AArch64)

---

## 1. Core Data Structure Relationships

```
Process Virtual Address Space — Complete Diagram:

  task_struct
    ├── mm: mm_struct*  ──────────────────────────────────────────────┐
    │                                                                   │
    └── thread_info                                                    │
                                                                       ▼
                                                            mm_struct
                                                            ├── pgd: pgd_t*   → ARM64: TTBR0_EL1
                                                            ├── mm_mt         → Maple Tree of VMAs
                                                            ├── context.id    → ASID (generation:value)
                                                            ├── mmap_lock     → rw_semaphore
                                                            ├── mm_users      → atomic (threads)
                                                            ├── mm_count       → atomic (kernel refs)
                                                            ├── start_code/end_code
                                                            ├── start_data/end_data
                                                            ├── start_brk/brk (heap)
                                                            ├── start_stack
                                                            └── rss_stat

VMA Chain (from mm_mt / rb_tree):
  vm_area_struct [0x400000-0x401000] r-xp (text)
    ├── vm_mm: → mm_struct
    ├── vm_flags: VM_READ|VM_EXEC
    ├── vm_page_prot: AP=EL0_RO, UXN=0
    ├── vm_file: → struct file (elf binary)
    ├── vm_ops: → file_vm_ops {.fault=filemap_fault}
    └── anon_vma: NULL (file-backed)
    
  vm_area_struct [0x600000-0x700000] rw-p (heap)
    ├── vm_mm: → mm_struct
    ├── vm_flags: VM_READ|VM_WRITE
    ├── vm_page_prot: AP=EL0_RW, UXN=1
    ├── vm_file: NULL (anonymous)
    ├── vm_ops: NULL
    └── anon_vma: → struct anon_vma (for rmap)

Page Tables (TTBR0_EL1 → mm->pgd):
  PGD[pgd_index(VA)] → PUD table
  PUD[pud_index(VA)] → PMD table
  PMD[pmd_index(VA)] → PTE table
  PTE[pte_index(VA)] = [PA[47:12] | UXN | PXN | AP | SH | AttrIndx | AF | valid]

Physical Pages:
  struct page (in vmemmap array)
    ├── flags: PG_locked|PG_dirty|PG_active|...
    ├── _refcount: reference count
    ├── _mapcount: PTE reference count
    ├── mapping: → address_space (file) or anon_vma (anon)
    ├── index: file offset or VA/PAGE_SIZE
    └── lru: list_head (LRU list)
```

---

## 2. ARM64 Virtual Address Space Layout

```
48-bit VA (TTBR1_EL1 = kernel, TTBR0_EL1 = user):

  User (TTBR0, 0x0000_xxxx):
  ┌────────────────────────────────────────┐
  │ 0x0000_0000_0000_0000                  │ NULL (unmapped)
  │ 0x0000_0000_0040_0000                  │ ELF load (PIE ASLR base ~64MB+random)
  │          .text, .rodata, .data, .bss   │
  │          heap → (grows up)             │
  │          shared libraries              │
  │          ← mmap allocations (grows ↓) │
  │          stack (grows down ↓)          │
  │ 0x0000_FFFF_FFFF_FFFF                  │ end of user VA
  └────────────────────────────────────────┘

  Kernel (TTBR1, 0xFFFF_xxxx):
  ┌────────────────────────────────────────┐
  │ 0xFFFF_0000_0000_0000  PAGE_OFFSET     │ Linear map start (= __va(0))
  │   [direct mapped RAM]                  │ PA 0 → 0xFFFF_0000_0000_0000
  │ 0xFFFF_8000_0000_0000  VMALLOC_START   │ vmalloc / ioremap
  │ 0xFFFF_B000_0000_0000  PCI_IO_START    │ PCI I/O
  │ 0xFFFF_E000_0000_0000  VMEMMAP_START   │ struct page array
  │ 0xFFFF_FFFF_8000_0000  kimage_vaddr    │ Kernel image (.text, .data, .bss)
  │ 0xFFFF_FFFF_FFFF_FFFF                  │ end of kernel VA
  └────────────────────────────────────────┘
```

---

## 3. Page Fault Decision Tree

```
CPU raises fault (Translation/Permission/AF fault on ARM64)
  │
  ▼
do_page_fault(far, esr, regs)
  │
  ├─ find_vma(mm, far):
  │    ├─ No VMA found ──────────────────────→ bad_area() → SIGSEGV (SEGV_MAPERR)
  │    ├─ VMA starts AFTER far:
  │    │    ├─ VM_GROWSDOWN set → expand_stack() ──→ continue ↓
  │    │    └─ NO → bad_area() → SIGSEGV
  │    └─ VMA covers far: check permissions
  │         ├─ Write to non-writable VMA → bad_area() → SIGSEGV (SEGV_ACCERR)
  │         └─ OK → handle_mm_fault()
  │
  ▼
handle_mm_fault(vma, addr, flags)
  │
  ├─ hugetlb? → hugetlb_fault()
  └─ regular → __handle_mm_fault()
       │
       ├─ Ensure PGD/PUD/PMD exist (alloc if needed)
       └─ handle_pte_fault()
            │
            ├─ pte_none (no PTE):
            │    ├─ anonymous VMA → do_anonymous_page()
            │    │    ├─ READ → map zero page (minor)
            │    │    └─ WRITE → alloc zeroed page (minor)
            │    └─ file VMA → do_fault()
            │         ├─ READ → filemap_fault() (minor or major)
            │         ├─ PRIVATE WRITE → do_cow_fault() (copy from file)
            │         └─ SHARED WRITE → do_shared_fault() (dirty page cache)
            │
            ├─ pte_present && !pte_write (permission fault):
            │    └─ WRITE attempt → do_wp_page() (CoW)
            │         ├─ mapcount==0 → wp_page_reuse() (make writable in-place)
            │         └─ mapcount>0  → wp_page_copy() (allocate new page, copy)
            │
            ├─ pte_present && pte_protnone (NUMA hint):
            │    └─ do_numa_page() (migrate to better NUMA node)
            │
            └─ !pte_present && !pte_none (swap entry):
                 └─ do_swap_page() (swap-in, MAJOR fault)
```

---

## 4. System Call Summary

| System Call | Primary Effect | Lock | TLB Impact |
|---|---|---|---|
| mmap() | Create VMA | mmap_lock W | None (lazy) |
| munmap() | Remove VMA + PTEs | mmap_lock W | Full flush |
| mprotect() | Change VMA permissions | mmap_lock W | Flush mapped range |
| brk(extend) | Extend heap VMA | mmap_lock W | None |
| brk(shrink) | Remove heap pages | mmap_lock W | Flush removed range |
| mlock() | Pin pages, set VM_LOCKED | mmap_lock W | None |
| madvise(DONTNEED) | Remove pages immediately | mmap_lock W | Flush range |
| madvise(WILLNEED) | Prefault pages | mmap_lock R | None |
| madvise(FREE) | Lazily free pages | mmap_lock W | Deferred |
| mremap() | Move/resize VMA | mmap_lock W | Flush old range |

---

## 5. ARM64-Specific MM Details

```
ASID (Address Space Identifier):
  mm_context_t.id: atomic64_t
    [63:16] = generation counter (incremented on ASID rollover)
    [15:0]  = 8-bit or 16-bit ASID value
  TTBR0_EL1[63:48] = ASID at context switch
  
  ASID rollover: when 256/65536 ASIDs exhausted:
    Increment generation → generation mismatch → full TLB flush → reallocate
    
Context switch ARM64:
  check_and_switch_context():
    If ASID valid (generation matches): just write TTBR0_EL1 (no flush)
    If ASID invalid: new_context() → assign new ASID
  cpu_switch_mm(pgd, mm):
    TTBR0_EL1 = __pa(pgd) | (ASID << 48)
    ISB

PTE attributes set by Linux for anonymous pages:
  Normal memory: AttrIndx = 0 (MAIR index 0 = Normal, Inner WB, Outer WB)
  Shareability: SH = 0b11 (Inner Shareable = all CPUs see same cache state)
  Access Flag: AF = 1 (set by kernel; ARM64 may raise AF fault if hardware doesn't auto-set)
  User Execute Never: UXN = 1 for data, 0 for executable text
  Privilege Execute Never: PXN = 1 always for user mappings
  Not-Global: nG = 1 for user pages (ASID-tagged in TLB)

TLB flush after page table changes:
  Single page: TLBI VAE1IS, <VA>     (Inner-shareable, by VA+ASID, EL1)
  Full ASID:   TLBI ASIDE1IS, <ASID> (All entries for this ASID)
  Full kernel: TLBI VMALLE1IS        (All EL1 entries, all ASIDs)
  Required barrier: DSB ISH after TLBI before returning to user/continuing
```

---

## 6. Top 20 Interview Questions

**Q1: What is the difference between mm_users and mm_count in mm_struct?**
`mm_users`: count of user-space tasks using this mm (threads). When it drops to 0, the virtual address space is destroyed. `mm_count`: total reference count including kernel references. When it drops to 0, the mm_struct itself is freed. Always: mm_count >= mm_users.

**Q2: How does the kernel find a VMA given a virtual address?**
`find_vma(mm, addr)`: searches the maple tree (Linux 6.1+) or red-black tree using O(log N). Returns first VMA with `vm_end > addr`. The caller must check `vma->vm_start <= addr` to confirm the address is within the VMA.

**Q3: When two processes mmap the same file, do they share the same struct page objects?**
Yes (for MAP_SHARED). Both VMAs map to the same page cache pages (`address_space->i_pages`). The struct pages have `_mapcount > 0` tracking both mappings. For MAP_PRIVATE: initially shared (same page cache pages, read-only), but after CoW write: private pages per process.

**Q4: Why does ARM64 need both PXN and UXN in PTEs?**
`UXN` (User Execute Never): prevents EL0 (user mode) from executing this page — used for data pages. `PXN` (Privileged Execute Never): prevents EL1 (kernel mode) from executing this page. This enforces kernel W^X (kernel write implies no execute). Without PXN, kernel could execute user-mapped code (confused deputy attack). Without UXN, user could execute kernel-mapped data.

**Q5: What is the difference between a minor and major page fault?**
Minor: page is in memory (or can be zero-filled) without disk I/O. Major: page must be read from disk (swap or file). Tracked per-process in `/proc/pid/stat` fields 10 (minflt) and 12 (majflt).

**Q6: What happens to a process's page tables when it calls exec()?**
`exec_mmap()` creates a new mm_struct with new (empty) page tables. The old mm_struct is released via `mmput()`. The `pgd_alloc()` for the new mm_struct allocates a fresh PGD page. Since ARM64 uses separate TTBR1 for kernel, only the user portion (TTBR0) is affected — kernel mappings are unaffected.

**Q7: Why is the zero page mapped read-only initially?**
To defer physical page allocation. Until a process writes to anonymous memory, it gets the shared zero page (all zeros). All reads return zeros without allocating unique physical pages. On first write: permission fault → do_wp_page() → real allocation. This is critical for processes with large heaps they never fully use.

**Q8: How does mprotect(PROT_READ | PROT_EXEC) interact with ARM64 PTE bits?**
Sets `UXN=0` (user can execute), `AP=EL0_RO` (read-only). The kernel calls `change_protection()` to update existing PTEs. It also calls `flush_tlb_range()` → `TLBI VAE1IS` for each page to invalidate cached TLB entries that had old permissions. Without TLB flush, old "writable" TLB entries could persist and bypass the new protection.

**Q9: Can you explain the anon_vma hierarchy after fork()?**
After fork, parent's VMA and child's VMA each have their own `anon_vma`, but child VMA has an `anon_vma_chain` linking it to the PARENT's `anon_vma` too. Pages created before fork have `page->mapping = parent_anon_vma`. Walking parent's anon_vma rb_root finds BOTH parent VMA and child VMA. Pages created after fork by child point to child's anon_vma, found only in child's rb_root.

**Q10: What is vmemmap and why does ARM64 use it?**
`vmemmap`: contiguous virtual address array where `vmemmap[pfn]` = `struct page` for PFN `pfn`. Enables O(1) `pfn_to_page()` and `page_to_pfn()`. ARM64 uses the VMEMMAP_START region in kernel VA space (near `0xFFFF_E000_0000_0000` for 48-bit). The physical pages backing vmemmap are allocated early in boot for each NUMA node.

**Q11: Describe the difference between MAP_ANONYMOUS and MAP_PRIVATE | file mapping.**
MAP_ANONYMOUS: no file backing. First access = zero-filled page. Not in page cache. On eviction: goes to swap. MAP_PRIVATE (file): backed by file. First read: loads from file into page cache. First write: CoW from page cache page to private anonymous page. On clean page eviction: just discard (re-read from file on next fault). On dirty CoW page eviction: goes to swap.

**Q12: How does madvise(MADV_DONTNEED) work on anonymous memory?**
Calls `zap_page_range()`: walks PTEs, clears each PTE, calls `page_remove_rmap()` + `put_page()` for each page. TLB flush for the range. Physical pages freed if refcount drops to 0. Next access: page fault → fresh zero page. Content is permanently lost. Used by glibc `free()` for large anonymous chunks.

**Q13: What is `mmap_lock` and why does it have both read and write modes?**
`mmap_lock` is an `rw_semaphore` protecting the VMA list. Write mode (exclusive): `mmap()`, `munmap()`, `mprotect()`, `brk()`, `fork()` — these MODIFY the VMA structure. Read mode (shared): page fault handling, `/proc/pid/maps` reads, `ptrace()` — these only READ the VMA structure. Multiple page fault handlers can run concurrently (shared read lock), but VMA modification is exclusive. High contention with many threads on multi-core ARM64 servers.

**Q14: What is the purpose of the Access Flag (AF) in ARM64 PTEs?**
AF=1 means the page has been accessed. ARM64 can either hardware-set AF (FEAT_HAFDBS) or fault on AF=0 (software management). Linux uses AF for LRU aging: `pte_young()` checks AF; `ptep_clear_flush_young()` clears it. Pages with recently set AF are "young" → not candidates for reclaim. Used in the two-chance page replacement algorithm.

**Q15: Explain how do_wp_page() decides whether to copy a page or make it writable in-place.**
Checks `page_count(page) == 1` (only one holder). If true AND `trylock_page()` succeeds: `wp_page_reuse()` — just sets `AP[2]=0` (writable) in PTE, no copy. This works when this process is the only mapper (other process already CoW'd or unmapped). If `page_count > 1` or exclusive ownership can't be confirmed: `wp_page_copy()` — allocate new page, copy content, install new writable PTE.

---

## 7. Common Debugging Tools

```
Process memory inspection:
  /proc/<pid>/maps:          All VMAs: start-end perm offset dev ino file
  /proc/<pid>/smaps:         Detailed per-VMA: Rss, Pss, Shared_*, Private_*, Swap
  /proc/<pid>/smaps_rollup:  Sum of smaps (fast total RSS/PSS)
  /proc/<pid>/pagemap:       Per-page: PFN, swapped, present (for privileged users)
  pmap -x <pid>:             userspace tool wrapping /proc/pid/maps
  
VM statistics:
  /proc/vmstat:              page faults (pgfault/pgmajfault), reclaim, swap
  /proc/meminfo:             MemFree, MemAvailable, Cached, Dirty, Writeback
  
Kernel memory tools:
  kmemleak:                  slab leak detector (CONFIG_DEBUG_KMEMLEAK)
  kasan:                     kernel address sanitizer (heap OOB, UAF)
  kmem_cache_info:           /sys/kernel/slab/ per-cache stats
  
ARM64-specific:
  perf stat -e cache-misses: cache miss rates
  perf mem:                  memory access profiling (D-TLB miss, L1D miss)
  perf stat -e dTLB-misses:  D-TLB miss count
  
  /proc/cpuinfo:             cache sizes, ASID bits
  arch_timer performance:    ARM64 can track page table walks via PMU events
```

---

## 8. Quick Reference Cheat Sheet

| Conversion | Code |
|---|---|
| VA → PFN | `__pa(va) >> PAGE_SHIFT` |
| PFN → struct page | `pfn_to_page(pfn)` = `vmemmap + pfn` |
| struct page → PA | `page_to_phys(p)` = `page_to_pfn(p) << PAGE_SHIFT` |
| PA → KVA | `__va(pa)` = `(pa) + PAGE_OFFSET` |
| KVA → PA | `__pa(va)` = `(va) - PAGE_OFFSET` |
| KVA → struct page | `virt_to_page(va)` |
| struct page → KVA | `page_address(p)` |

| mm_struct Lifecycle | Trigger |
|---|---|
| Created | `fork()` (dup_mm) or `execve()` |
| mm_users++ | New task shares mm (fork/thread) |
| mm_users-- | Task exits (exit_mm) |
| mm_count++ | Kernel holds reference (mmget) |
| mm_count-- | Kernel drops reference (mmdrop) |
| Freed | mm_count drops to 0 → __mmdrop() |

| ARM64 PTE Field | Position | Effect |
|---|---|---|
| valid | bit[0] | Entry is valid (mapped) |
| AP[2:1] | bits[7:6] | Read/write and EL0 access |
| AF | bit[10] | Access flag (LRU tracking) |
| nG | bit[11] | Not-global (ASID tagged) |
| UXN | bit[54] | User execute never |
| PXN | bit[53] | Privileged execute never |
| SW dirty | bit[55] | Software dirty tracking |
