# CMA: Contiguous Memory Allocator

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
CMA: Contiguous Memory Allocator
  Problem: video decoders, camera ISPs, GPU codecs need large physically
           contiguous memory (256MB–2GB) for frame buffers / codec buffers
  
  Two naive approaches and their problems:
    A. memblock_reserve at boot: reserve large contiguous regions permanently
       Problem: memory is WASTED when device not in use (movie not playing)
    
    B. dma_alloc_coherent at runtime: buddy allocator
       Problem: after the system runs for a while, physical memory fragments.
       Order-10 buddy (4MB max) cannot satisfy a 256MB request.
       Even with compaction, large contiguous allocs fail under load.
  
  CMA solution: "reserve but borrow"
    Reserve a large contiguous physical region at boot
    The region is populated with MOVABLE pages (user process pages, page cache)
    These borrowed pages can serve normal allocations most of the time
    When device needs the memory: evict/migrate all pages out of CMA region
                                   Return physically contiguous region to device
    When device done: CMA region is returned to normal page allocator
  
  Result: best of both worlds
    Memory not wasted (borrowed by normal allocations)
    Large contiguous buffers available on demand
    No fragmentation problem (region was never fragmented — MOVABLE pages can be moved out)

ARM64 and CMA:
  ARM64 SoCs (mobile/embedded): very common to have 512MB CMA for camera/video
  ARM64 servers: CMA used for large guest VM memory, GPU, or RDMA NIC buffers
  Device tree: `linux,cma-default` property or `cma=NNNm` kernel parameter
```

---

## 2. CMA Architecture

```
CMA regions — two types:
  1. Global CMA (cma_default):
     Created from kernel command line: cma=256m
     All devices without their own CMA region fall back to this
     Physical: highest contiguous free region of this size at boot

  2. Per-device CMA:
     Defined in device tree or ACPI tables
     Example DTS:
       reserved-memory {
           #address-cells = <2>;
           #size-cells = <2>;
           ranges;
           
           linux,cma {
               compatible = "shared-dma-pool";
               reusable;
               reg = <0x0 0x80000000 0x0 0x10000000>; // 256MB at 2GB PA
               linux,cma-default;
           };
           
           camera_reserved: camera@90000000 {
               compatible = "shared-dma-pool";
               no-map;               // reserved, NOT given to buddy
               reg = <0x0 0x90000000 0x0 0x8000000>; // 128MB for camera
           };
       };
       
       camera@40000000 {
           memory-region = <&camera_reserved>;
       };

CMA data structure:
  struct cma {
      unsigned long   base_pfn;     // start PFN of CMA region
      unsigned long   count;         // number of pages in region
      unsigned long   *bitmap;       // allocation bitmap
      unsigned int    order_per_bit; // granularity
      spinlock_t      lock;
      struct page     *pages;        // first struct page
  };
  
  Global array: cma_areas[MAX_CMA_AREAS]; (up to 64 CMA areas on ARM64)
  cma_get_size() / cma_get_base(): access area properties
```

---

## 3. CMA Allocation Implementation

```c
/* mm/cma.c */

struct page *cma_alloc(struct cma *cma, unsigned long count,
                        unsigned int align, bool no_warn)
{
    // count: number of pages
    // align: alignment in pages (must be power of 2)
    
    // 1. Find a free region in CMA bitmap:
    pfn = cma_bitmap_find_area(cma, count, align);
    // Scans cma->bitmap for 'count' consecutive 0-bits (free pages)
    // If no free region: compaction first, then retry
    
    // 2. Set bits as "allocated" in bitmap:
    bitmap_set(cma->bitmap, bit_offset, count);
    
    // 3. Isolate pages from buddy allocator (prevent new allocations):
    start_pfn = cma->base_pfn + pfn;
    isolate_migratepages_block(cc, start_pfn, ...):
        // For each page in range:
        //   If on LRU list: set_bit(PG_isolated, page->flags)
        //                   Remove from LRU (move to migratepages list)
    
    // 4. Migrate pages OUT of the CMA region:
    migrate_pages(&migratepages, alloc_migrate_page, ...):
        // For each isolated page:
        //   Find new physical page: alloc_page(GFP_HIGHUSER_MOVABLE)
        //                           NOT from CMA (avoid recursion)
        //   migrate_page(old_page, new_page):
        //     a. unmap old page: try_to_unmap(old_page)
        //                        Walks rmap, clears PTEs
        //     b. copy content: copy_highpage(new_page, old_page)
        //     c. install new PTEs: move_to_new_page(new_page, ...)
        //     d. flush TLB: tlb_flush_mmu()
        //                   ARM64: TLBI VAE1IS per old PTE
        //     e. Free old page back to buddy (old_page now free in CMA)
    
    // 5. After migration: all pages in [start_pfn, start_pfn+count) are free
    //    Check: each page must be free (on buddy free list)
    
    // 6. Remove pages from buddy (take ownership):
    alloc_contig_range(start_pfn, start_pfn + count, MIGRATE_CMA, gfp):
        // Calls __alloc_contig_migrate_range which calls alloc_contig_pages
        // Takes the pages off buddy free lists
    
    // 7. Return first page:
    return pfn_to_page(start_pfn);
}

void cma_release(struct cma *cma, struct page *pages, unsigned long count)
{
    pfn = page_to_pfn(pages);
    
    // 1. Return pages to buddy as MIGRATE_CMA type:
    __free_pages(pages, get_order(count * PAGE_SIZE));
    // Actually: free_contig_range(pfn, count) → frees pages individually to buddy
    // Pages go back into MIGRATE_CMA migration type list
    
    // 2. Clear bitmap:
    bitmap_clear(cma->bitmap, bit_offset, count);
    
    // Pages are now available for normal allocations (movable/page cache)
    // Next cma_alloc will have to migrate them out again
}

dma_alloc_from_contiguous(dev, count, align, no_warn):
    // Wrapper: chooses correct CMA area for device, calls cma_alloc()
    cma = dev_get_cma_area(dev);  // per-device CMA or default
    return cma_alloc(cma, count, align, no_warn);
```

---

## 4. ARM64 Boot-time CMA Setup

```
ARM64 CMA initialization sequence:

1. early_init_fdt_scan_reserved_mem() [arm64/kernel/setup.c]:
   Parses /reserved-memory from device tree
   Registers CMA regions: rmem_cma_setup()

2. arm64_memblock_init() [arch/arm64/mm/init.c]:
   memblock_reserve(cma_base, cma_size): reserves from memblock
   (prevents early bootmem from using it)

3. cma_init_reserved_mem() / cma_declare_contiguous():
   Called during mm_init / driver_init phase
   Configures struct cma metadata
   CMA region pages: added to buddy as MIGRATE_CMA type:
     for each page in cma region:
       page->migratetype = MIGRATE_CMA
       add to free_area[order].free_list[MIGRATE_CMA]
   
4. Pages available to normal allocations:
   alloc_pages(GFP_HIGHUSER_MOVABLE): can get MIGRATE_CMA pages
   These pages serve normal user allocations
   They are MOVABLE: can be migrated when cma_alloc() is called

CMA and /proc/buddyinfo:
  Shows CMA pages in MIGRATE_CMA column (not shown by default in buddyinfo)
  /proc/vmstat has:
    cma_alloc_success: number of successful CMA allocations
    cma_alloc_fail:    number of failed CMA allocations (fragmentation)
  
  /sys/kernel/debug/cma/: per-CMA area statistics (debugfs)
    count: total pages
    used: currently allocated pages

CMA failure modes:
  cma_alloc fails when:
    1. Pages cannot be migrated (PG_mlocked pages: mlock() pins them in place)
       mlock'd pages are NOT movable → CMA allocation fails
    2. Migration fails for other reasons (page in use by kernel, not user)
    3. Fragmentation within CMA region itself (allocation pattern leaves holes)
  
  On failure:
    cma_alloc returns NULL
    Device driver falls back to dma_alloc_coherent() (may fail too if fragmented)
    Or falls back to using smaller non-contiguous buffers (if supported)
```

---

## 5. Interview Questions & Answers

**Q1: Why can CMA use MOVABLE pages? What types of pages cannot be placed in CMA?**

CMA works because MOVABLE pages can be physically relocated without breaking program semantics. The key requirement: the page must have a complete reverse-mapping (rmap) so that all PTEs pointing to it can be updated to point to the new physical location.

**Pages that CAN be in CMA (movable)**:
- Anonymous user pages (single process or CoW): full anon_vma rmap
- File-backed page cache pages: can be discarded (re-read from file) or moved
- User stack and heap pages: all have PTEs tracked via anon_vma
- MIGRATE_MOVABLE pages: explicitly marked for mobility

**Pages that CANNOT be in CMA (unmovable)**:
- Kernel slab pages (MIGRATE_UNMOVABLE): internal kernel data structures have direct pointers (not PTE-mediated). Moving them would invalidate kernel pointers.
- Kernel stack pages: kernel code holds stack pointer directly
- `mlock()`'d pages (PG_mlocked): explicitly pinned by user request — cannot migrate
- Pages currently under DMA (pinned for DMA transfer)
- Pages with `get_page()` elevated refcount from kernel pin (like vmscan's working set reference)
- Huge pages (if not THP, which are movable under some conditions)
- pinned RDMA pages (get_user_pages with FOLL_PIN)

**Q2: What happens to performance during a cma_alloc() call? Is it suitable for real-time use?**

CMA allocation can be significantly slow and non-deterministic:

1. **Page scanning**: scan the entire CMA region to find isolatable pages
2. **Page migration**: for each occupied page — unmap PTEs, copy content (~4KB per page), install new PTEs, TLB flush. For a 256MB allocation with 64K pages occupied: potentially 64K page copies = 256MB memcpy.
3. **TLB shootdowns**: multiple TLBI broadcasts on ARM64 (each broadcast takes ~µs on large CPU clusters)

Worst case: a few hundred milliseconds for a large CMA allocation.

CMA is **NOT suitable for real-time or latency-sensitive contexts**:
- Don't call from interrupt handlers (GFP_ATOMIC would fail anyway)
- Don't call while holding critical locks
- Pre-allocate CMA buffers at driver probe time if possible
- Use carveout memory (no-map) for hard real-time devices where latency matters

For camera/video pipelines: typically allocate all CMA buffers at camera open, keep them allocated for the duration of the streaming session, release at camera close.

---

## 6. Quick Reference

| Concept | Detail |
|---|---|
| CMA reserve kernel param | `cma=256m` (256MB global CMA) |
| Device tree | `/reserved-memory` with `reusable` property |
| Pages in CMA | MIGRATE_CMA type, movable |
| Allocation API | `cma_alloc(cma, count, align)` |
| Device driver API | `dma_alloc_from_contiguous()` |
| Release API | `cma_release(cma, page, count)` |
| Failure cause | mlock'd/pinned pages, internal fragmentation |
| Debug | `/sys/kernel/debug/cma/`, `/proc/vmstat` cma_alloc_* |
