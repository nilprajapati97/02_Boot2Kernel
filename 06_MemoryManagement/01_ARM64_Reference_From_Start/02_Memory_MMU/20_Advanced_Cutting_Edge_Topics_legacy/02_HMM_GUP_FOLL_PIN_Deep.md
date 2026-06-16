# HMM and GUP/FOLL_PIN Deep Dive

**Category**: Advanced and Cutting-Edge Topics  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
HMM (Heterogeneous Memory Management):
  Problem: GPU/accelerator has its own memory (device-local DRAM)
           CPU has system RAM
           Application allocates: void *ptr = malloc(1MB) [CPU memory]
           GPU wants to: DMA-read from ptr (needs physical address)
           Issue: CPU virtual address != GPU accessible memory address
                  CPU page tables: not accessible by GPU
  
  Old approach (before HMM):
    1. Manually pin: get_user_pages(ptr) → get physical addresses
    2. Copy data to GPU-mapped buffer
    3. Run GPU kernel on copy
    4. Copy results back
    → Extra copies, bandwidth wasted, complex application code
  
  HMM approach:
    1. GPU driver registers: hmm_mirror_register(mm)
    2. hmm_range_fault(range): walks CPU page tables for GPU
       Returns physical addresses accessible by GPU
    3. GPU can DMA to/from CPU memory directly (if IOMMU/SMMU allows)
    4. If CPU page migrates: MMU notifier callback → GPU invalidates mapping
    5. Can also migrate: MIGRATE_VMA moves pages to device-private memory
       Pages "live" on GPU, mapped in CPU page table as device-private PTE
       CPU access: fault → migrate back to system RAM
  
  ARM64 relevance: Mali GPU, etnaviv GPU, Qualcomm Adreno (via OpenCL/Vulkan)
  Also: AI accelerators (ARM Ethos-N) with unified memory model
```

---

## 2. HMM Architecture on ARM64

```
struct hmm_range:
  struct mm_struct *notifier_seq;   // mm being mirrored
  struct mmu_notifier *notifier;    // registered notifier
  unsigned long start;              // VA range start
  unsigned long end;                // VA range end
  hmm_pfn_t *pfns;                  // output: physical frames
  const struct hmm_range_flags *flags;  // access requirements
  u64 default_flags;                // FAULT_FLAG_WRITE, etc.
  u64 pfn_flags_mask;               // which flags to return per pfn

hmm_range_fault(range):
  // Walk CPU page tables for range [start, end)
  // For each page:
  //   1. Check PTE: is page present?
  //      No: fault it in (may allocate new page, swap in, etc.)
  //   2. Check permissions: read? write? requested by 'flags'?
  //      No write permission but write requested: COW fault
  //   3. Get physical address: page_to_phys(pte_page(pte))
  //   4. Store in range->pfns[i]
  // Returns: number of pages successfully faulted in
  //          Or: -EAGAIN if page table changed during fault
  //                (retry: snapshot mechanism with notifier_seq)
  
  // Retry pattern (required!):
  do {
      range->notifier_seq = mmu_interval_read_begin(range->notifier);
      ret = hmm_range_fault(range);
      if (ret == -EAGAIN)
          continue;  // page table changed: retry
  } while (mmu_interval_read_retry(range->notifier, range->notifier_seq));
  // Atomically: "no MMU change occurred during fault" is guaranteed
```

### 2.1 MIGRATE_VMA — Moving Pages to Device Memory

```c
// include/linux/migrate.h + mm/migrate_device.c

// Device private memory: pages that "exist" in GPU DRAM
// CPU page table: device-private PTE (present=0, device-private encoding)
// CPU access to device-private page: fault → migrate back to CPU

struct migrate_vma {
    struct vm_area_struct *vma;
    unsigned long *dst;    // array: destination PFNs (device private PFNs)
    unsigned long *src;    // array: source PFNs (CPU-side)
    unsigned long cpages;  // count: pages to migrate to device
    unsigned long npages;  // total pages in range
    unsigned long start;   // VA range start
    unsigned long end;     // VA range end
    struct page *pgmap_owner; // device memory owner (struct dev_pagemap)
    unsigned long flags;   // MIGRATE_VMA_SELECT_SYSTEM or DEVICE_PRIVATE
};

// Migration steps (CPU→GPU):
migrate_vma_setup(&args):
    // Walk CPU page tables for [start, end)
    // For each 4KB page:
    //   - Get page (fault in if needed)
    //   - Set src[i] = pfn | MIGRATE_PFN_MIGRATE
    //   - Optionally: set src[i] = 0 if page should NOT move

migrate_vma_pages(&args):
    // Driver callback: copy CPU pages to GPU device memory
    // For each page in src[]:
    //   GPU DMA: copy CPU_PFN → device_PFN
    //   Set dst[i] = device_pfn | MIGRATE_PFN_VALID
    // CPU page table: PTE → device_private PTE (pointing to device PFN)

migrate_vma_finalize(&args):
    // Install device-private PTEs in CPU page table
    // Free original CPU pages (put_page)
    // CPU TLB flush: flush_tlb_range(vma, start, end)
    // ARM64: TLBI ASIDE1IS (flush by ASID, inner shareable broadcast)

// Now: CPU page table has device-private PTEs for these pages
// CPU read from these addresses → page fault → migrate back to CPU RAM
```

---

## 3. MMU Notifiers

```c
// include/linux/mmu_notifier.h

// MMU notifiers: callbacks invoked when CPU page table changes
// GPU/device driver registers to be notified

struct mmu_notifier_ops {
    // Called before PTE/PMD change (pre-invalidate):
    void (*invalidate_range_start)(struct mmu_notifier *mn,
                                   const struct mmu_notifier_range *range);
    // Called after PTE/PMD change (post-invalidate):
    void (*invalidate_range_end)(struct mmu_notifier *mn,
                                 const struct mmu_notifier_range *range);
    // Called when mm is about to be destroyed:
    void (*release)(struct mmu_notifier *mn, struct mm_struct *mm);
};

// GPU driver implementation:
my_gpu_invalidate_range_start(mn, range):
    // Invalidate GPU TLB entries for range->start .. range->end
    // ARM64 SMMU: send TLBI command via SMMU command queue
    //   SMMU_TLBI_NH_VA (Non-secure, Hyp-mode, VA) or
    //   SMMU_TLBI_NH_ASID (by ASID)
    // Wait for SMMU to complete invalidation before returning

// When does invalidate_range_start fire?
//   - mprotect() changes protection
//   - munmap() removes mapping
//   - mremap() moves mapping
//   - fork() COW setup
//   - page migration
//   - madvise(MADV_DONTNEED)
// Basically: any CPU page table modification
```

---

## 4. GUP (Get User Pages) and FOLL_PIN

```
get_user_pages (GUP): kernel API to pin user memory for kernel use
  Use cases:
    - DMA: NIC/storage device needs physical address of user buffer
    - RDMA: InfiniBand registers user memory for direct hardware access
    - io_uring: zero-copy network/storage I/O
    - V4L2: camera DMA buffers
    - io_mem: any char device that DMA's to user memory

Traditional GUP (get_user_pages):
  get_user_pages(start, nr_pages, gup_flags, pages[]):
    Increments page->_refcount for each page
    Pins page in memory: page won't be freed while refcount > 1
    Problem: page CAN migrate (NUMA migration, compaction moves page)
    → physical address returned may become stale after migration
    → DMA to stale PA: data corruption (catastrophic!)
    
    Root cause: _refcount only prevents FREE, not MOVE

FOLL_PIN (new, safer alternative):
  pin_user_pages(start, nr_pages, gup_flags, pages[]):
    Uses: page->_refcount elevated by GUP_PIN_COUNTING_BIAS (1024)
    OR: folio->_pincount separate counter (for large folios)
    
    Semantic difference:
      _refcount++ via get_page(): "someone is looking at this page"
                                   CAN be migrated by kernel (update mapcount)
      _refcount += GUP_PIN_COUNTING_BIAS: "DMA in progress"
                                           CANNOT be migrated
                                           CANNOT be given to another process
    
    Memory constraints with FOLL_PIN:
      Pinned pages CANNOT be on CMA (Contiguous Memory Allocator) regions
      Pinned pages CANNOT be in ZONE_MOVABLE
      Reason: CMA/MOVABLE assume pages can be migrated → pinning breaks this
      
      FOLL_LONGTERM: even stricter — page CANNOT move for extended duration
      ZONE_MOVABLE restriction: alloc for FOLL_LONGTERM refuses ZONE_MOVABLE
    
    unpin_user_pages(pages[], nr_pages):
      Decrements refcount by GUP_PIN_COUNTING_BIAS
      After: page can be migrated again

ARM64 GUP optimization — gup_fast():
  Lockless PTE walk (no mmap_lock held):
  
  gup_fast(start, nr_pages, gup_flags, pages[]):
    Disables IRQ (to prevent context switch mid-walk)
    Walks page tables: pgd → pud → pmd → pte
    ARM64: check each PTE for:
      PTE_VALID (bit[0]=1): page is present
      PTE_USER: page is user-accessible
      PTE_RDONLY: if FOLL_WRITE requested but PTE is read-only → skip
    For valid PTEs: increment _refcount + store in pages[]
    
    Race condition: between PTE read and refcount increment
      Another thread unmaps the page (PTE cleared)
      gup_fast() got stale PTE reference
    
    ARM64 solution:
      After incrementing refcount: re-read PTE
      If PTE changed: drop the reference (incorrect PTE snapshot)
      Fall back to gup_slow() (with mmap_lock) for this page
  
  gup_slow(start, nr_pages, gup_flags, pages[]):
    Holds mmap_lock (read) to prevent VMA/PTE changes
    Walks page tables under lock → safe but slower
    Can trigger demand paging: faults in pages if not present
```

---

## 5. ARM64 IOMMU/SMMU Integration with HMM

```
ARM SMMU (System MMU) + HMM:

SMMU: translates device DMA addresses → physical addresses
  Device (GPU/NIC) issues DMA to: IOVA (I/O Virtual Address)
  SMMU: IOVA → PA translation (using SMMU page tables)
  SMMU page tables: separate from CPU MMU page tables

HMM + SMMU coordination:
  1. GPU driver: calls hmm_range_fault() → gets PA list for CPU VAs
  2. GPU driver: programs SMMU page tables: IOVA → PA
  3. GPU issues DMA: IOVA → SMMU → PA → DRAM
  
  Coherency requirement:
    CPU updates PTE (migration, COW, mprotect):
      → MMU notifier fires: invalidate_range_start()
      → GPU driver: invalidate SMMU TLBI for affected IOVA range
         SMMU CMD_TLBI_NH_VA: per-VA SMMU TLB invalidation
      → After SMMU flush: CPU PTE update proceeds

  ARM64 SMMU-v3 command queue:
    Commands: TLBI_NH_VA, TLBI_NH_ASID, SYNC
    Atomicity: CMD_SYNC ensures all prior commands complete
    
    // Pseudocode for SMMU TLB flush:
    smmu_cmdq_issue_cmd(smmu, CMD_TLBI_NH_VA):
        Write command to SMMU CMDQ_PROD pointer
        Ring doorbell (write SMMU_CMDQ_CONS register)
    smmu_cmdq_issue_cmd(smmu, CMD_SYNC):
        Write SYNC command
        Poll: wait until SMMU_CMDQ_CONS catches up to PROD
    // Now: SMMU TLB has been flushed for the VA range
```

---

## 6. Interview Q&A

**Q1: What problem does HMM solve that traditional DMA mappings don't?**
Traditional DMA: application must explicitly pin memory (`mlock` or device-specific API), pass physical addresses to driver, manage lifetime carefully. HMM enables: CPU virtual addresses to be used as the common currency between CPU and device. The driver uses `hmm_range_fault()` to walk CPU page tables and obtain physical addresses. When the CPU page table changes (migration, swap, mprotect): MMU notifiers automatically notify the GPU driver to invalidate its SMMU/IOMMU mappings. This makes GPU access to CPU memory transparent to the application — no explicit pinning or buffer management required.

**Q2: Why can't FOLL_PIN pages exist in ZONE_MOVABLE?**
ZONE_MOVABLE exists specifically so the kernel can migrate all pages in it (for memory hotplug, defragmentation). FOLL_PIN means "DMA in progress to this physical address" — if the page migrated, the physical address the DMA was targeting is now wrong → data corruption. The buddy allocator enforces: `FOLL_LONGTERM` allocations refuse ZONE_MOVABLE. Similarly, CMA (Contiguous Memory Allocator) reserves movable memory — pinned pages cannot come from CMA regions since CMA's compaction algorithm assumes it can move anything in its region.

**Q3: What is the race condition in gup_fast() and how is it handled on ARM64?**
gup_fast() reads a PTE and then increments the page's `_refcount` — these are two non-atomic operations. Between reading the PTE and incrementing the refcount: another thread could unmap the page (clear the PTE) and the buddy could free the page. Result: gup_fast() holds a refcount to a page that was freed and potentially reallocated. ARM64 solution: after incrementing refcount, re-read the PTE. If the PTE changed (or became invalid): release the refcount immediately and fall through to gup_slow() (which holds `mmap_lock` to prevent PTE changes during the walk). The load-acquire (`ldar`) semantics on ARM64 ensure proper ordering of the PTE re-read after the store-release of the refcount increment.

**Q4: What ARM64 TLB instruction is used when HMM/MMU notifier invalidates SMMU mappings?**
The ARM SMMU-v3 uses a separate command queue for TLB operations. For HMM MMU notifier callbacks: `CMD_TLBI_NH_VA` (TLB Invalidate by VA, Non-secure, Hypervisor-mode) is used to invalidate specific IOVAs in the SMMU TLB. Followed by `CMD_SYNC` to ensure the invalidation completes before continuing. The CPU side (for CPU TLB): `TLBI VAE1IS, Xt` (inner shareable, broadcast). Both must complete before the CPU page table change proceeds. The `invalidate_range_start()` callback handles the SMMU flush, and the CPU TLB flush happens as part of the normal page table update path.

**Q5: Explain the GUP_PIN_COUNTING_BIAS approach versus a simple refcount increment.**
A simple `get_page()` (refcount +1) only prevents the page from being freed — migration can still proceed (migrating updates the PTE and transfers the refcount). FOLL_PIN uses `GUP_PIN_COUNTING_BIAS = 1024`: incrementing refcount by 1024 signals "DMA pinned" to the migration machinery. `PageMovable()` check: if refcount >= GUP_PIN_COUNTING_BIAS on any page type, migration aborts. This is an out-of-band signal that doesn't require a separate data structure. For large folios: `folio->_pincount` is a separate atomic counter (to avoid overflow concerns with GUP_PIN_COUNTING_BIAS on large allocations). `unpin_user_pages()` decrements by GUP_PIN_COUNTING_BIAS (or folio->_pincount) to release the DMA pin.

---

## 7. Quick Reference

| API | What It Does |
|---|---|
| `hmm_range_fault(range)` | Walk CPU PTs, return PA list for device |
| `migrate_vma_setup()` | Prepare pages for device migration |
| `migrate_vma_pages()` | Driver copies CPU→device memory |
| `migrate_vma_finalize()` | Install device-private PTEs, flush TLB |
| `pin_user_pages()` | FOLL_PIN: pin + prevent migration |
| `unpin_user_pages()` | Release FOLL_PIN |
| `get_user_pages()` | Legacy: pin but migration still possible |

| SMMU Command | Purpose |
|---|---|
| `CMD_TLBI_NH_VA` | Invalidate SMMU TLB by VA |
| `CMD_TLBI_NH_ASID` | Invalidate SMMU TLB by ASID |
| `CMD_SYNC` | Wait for all prior commands to complete |
