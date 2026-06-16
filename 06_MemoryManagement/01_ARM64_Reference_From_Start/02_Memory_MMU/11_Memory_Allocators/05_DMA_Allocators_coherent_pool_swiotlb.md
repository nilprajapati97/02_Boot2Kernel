# DMA Allocators: dma_alloc_coherent, dma_pool, swiotlb

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
DMA (Direct Memory Access): hardware device reads/writes memory WITHOUT CPU involvement
  DMA buffer requirements:
    1. Physical address known (device uses PA, not VA)
    2. Physically contiguous (most DMA controllers: 1 base addr + length)
    3. Cache coherent: CPU reads must see device writes and vice versa
    4. May need to be below 4GB (32-bit DMA devices)

DMA coherency challenge on ARM64:
  ARM64 caches: write-back cacheable for Normal memory
  When CPU writes to buffer: data in L1/L2 cache, NOT in DRAM
  When device reads via DMA: reads DRAM directly → STALE DATA!
  
  Similarly:
  When device writes via DMA: writes to DRAM
  When CPU reads: may read cached (old) data → STALE DATA!
  
  Solutions:
    A. Hardware cache coherency: AMBA ACE/ACE-Lite interface
       ARM CoreLink CCI (Cache Coherency Interconnect): device participates in
       ARM coherency domain. Device reads/writes go through coherency fabric.
       Most SoC-internal devices (eMMC, USB) on ARM64: FULLY COHERENT
       → coherent DMA works without software cache management
    
    B. Software cache management (non-coherent devices):
       dma_map_single(): flush cache lines before DMA read, invalidate before DMA write
       Explicit: clflush, DC CIVAC (Clean+Invalidate by VA to PoC)
    
    C. Bounce buffers (swiotlb): device can't reach all memory (32-bit DMA limit)
       Allocate buffer below 4GB, copy data to/from actual buffer

dma_alloc_coherent: the main kernel API for coherent DMA buffers
  Returns: 1. virtual address (CPU access) + 2. dma_addr_t (device address)
  Guarantees:
    - Physically contiguous
    - Cache coherent between CPU and device
    - Accessible by device (within device's DMA mask)
```

---

## 2. dma_alloc_coherent Implementation

```c
/* include/linux/dma-mapping.h */

void *dma_alloc_coherent(struct device *dev, size_t size,
                          dma_addr_t *dma_handle, gfp_t gfp)
{
    return dma_alloc_attrs(dev, size, dma_handle, gfp, 0);
}

// Path without IOMMU (direct DMA):
dma_alloc_attrs() → __dma_alloc()
    → dma_direct_alloc():
        // 1. Determine DMA zone based on dev->coherent_dma_mask:
        //    64-bit DMA capable: GFP_KERNEL
        //    32-bit DMA limit:   GFP_DMA32 (allocate from ZONE_DMA32, below 4GB)
        
        // 2. Allocate physically contiguous pages:
        pages = alloc_pages(gfp | __GFP_ZERO, get_order(size));
        // get_order(size): smallest order such that (1 << order) * PAGE_SIZE >= size
        
        // 3. Set DMA handle (physical address for device):
        *dma_handle = dma_direct_map_page(dev, pages, 0, size, DMA_BIDIRECTIONAL);
        // = phys_to_dma(dev, page_to_phys(pages))
        // On ARM64: phys_to_dma = physical address - dma_pfn_offset
        //           (on most ARM64 SoCs: dma_pfn_offset=0, so dma_addr = phys_addr)
        
        // 4. Handle cache coherency:
        // Coherent device (most ARM64 SoC DMA masters): normal kmalloc memory
        //   Already coherent → no cache operations needed
        
        // Non-coherent: set NON-CACHEABLE attribute:
        //   arch_dma_set_uncached(page, size)
        //   Creates new mapping with Normal NC (non-cacheable) in kernel vmalloc
        //   ARM64: uses pgprot_dmacoherent = Normal NC
        //   Returns new VA for CPU access (non-cached)
        //   Device uses dma_handle (physical address) directly
        
        return (void *)va;  // CPU virtual address

// Path WITH IOMMU (most modern ARM64 servers):
dma_alloc_attrs() → iommu_dma_alloc():
    // 1. Allocate physical pages (doesn't need to be below 4GB!):
    pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
    
    // 2. Map into IOMMU: assign IOVA (I/O Virtual Address):
    iova = iommu_dma_alloc_iova(domain, size, dma_mask, dev);
    iommu_map(domain, iova, page_to_phys(pages), size, prot);
    // IOMMU creates its own page table: IOVA → PA
    // Device uses IOVA as its "physical address" (device virtual address)
    
    // 3. dma_handle = IOVA (not physical address!):
    *dma_handle = iova;
    
    // 4. CPU virtual address: normal kmalloc/vmalloc mapping
    return (void *)va;
```

---

## 3. Streaming DMA (dma_map_single)

```
Two types of DMA mapping:
  1. Coherent (persistent): dma_alloc_coherent / dma_free_coherent
     Buffer exists for device lifetime
     Always coherent (hardware or NC memory)
  
  2. Streaming (one-shot): dma_map_single / dma_unmap_single
     Existing kernel buffer (kmalloc'd, vmalloc'd page, stack)
     Map before DMA transfer, unmap after
     Cache operations done by map/unmap

dma_map_single(dev, virt_addr, size, direction):
  direction:
    DMA_TO_DEVICE:     CPU → Device (CPU fills buffer, device reads)
    DMA_FROM_DEVICE:   Device → CPU (device fills buffer, CPU reads)
    DMA_BIDIRECTIONAL: both directions

  Non-IOMMU path (direct DMA):
    DMA_TO_DEVICE:
      Cache operation: DC CIVAC (clean + invalidate)
        Flushes CPU cache lines to DRAM
        Device reads fresh data from DRAM
        Returns: dma_handle = phys_to_dma(virt_to_phys(virt_addr))
    
    DMA_FROM_DEVICE:
      Cache operation: DC IVAC (invalidate, no clean)
        Removes stale CPU cache entries
        After DMA completes: CPU reads from DRAM (device's written data)
        CRITICAL: don't touch buffer between map and unmap (would re-cache!)
    
    DMA_BIDIRECTIONAL:
      Cache operation: DC CIVAC (both directions, safest)

  ARM64 cache instructions:
    DC CIVAC, X0 (Clean+Invalidate to Point of Coherency, by VA):
      Writes dirty cache line to DRAM AND removes from cache
    DC IVAC, X0 (Invalidate by VA):
      Removes cache line without writing (safe only if we know data is clean)
    DSB SY (Data Synchronization Barrier):
      Wait for all cache ops to complete before proceeding

dma_unmap_single(dev, dma_handle, size, direction):
  DMA_FROM_DEVICE:
    No cache operation needed (cache was already invalidated at map time)
    Just unmap from IOMMU if applicable
  
  DMA_TO_DEVICE:
    No cache operation needed (cache was cleaned at map time)
  
  After unmap: CPU can safely access the buffer with normal cached accesses

Scatter-Gather:
  dma_map_sg(dev, sgl, nents, direction): maps struct scatterlist array
  Each sg entry: page + offset + length
  Returns: number of successfully mapped sg entries
  Device uses sgl[i].dma_address for each entry
```

---

## 4. swiotlb (Software I/O TLB / Bounce Buffers)

```
Problem: device can only DMA to/from physical addresses below 4GB
         (legacy 32-bit DMA devices, e.g., some old PCIe devices)
         Modern ARM64 server: RAM may extend to 64GB+
         
         If kernel buffer is at PA = 0x2000000000 (8GB): device can't reach it!

swiotlb solution: bounce buffers
  At boot: reserve a region of memory below 4GB
    swiotlb_init(): alloc_bootmem_low_pages(SLOP_SIZE)
    ARM64: allocates from ZONE_DMA32 during early boot
    Default size: 64MB (configurable with swiotlb=NNN boot param)
  
  When dma_map_single(dev, high_pa_buf, size, direction):
    swiotlb_tbl_map_single():
      1. Find free slot in bounce buffer (below 4GB)
      2. DMA_TO_DEVICE: copy src buffer → bounce buffer (memcpy)
         Device sees and reads the bounce buffer
      3. Return dma_handle = bounce buffer physical address (below 4GB)
  
  When dma_unmap_single():
    DMA_FROM_DEVICE: copy bounce buffer → original buffer (memcpy)
    Free the bounce buffer slot
  
  Performance impact: extra memcpy for every DMA transfer
  Modern ARM64 avoids: 
    IOMMU: translates 32-bit IOVA → high PA (no copy needed)
    Hardware coherent DMA masters: 64-bit capable (no bounce needed)
  
  When swiotlb is needed on ARM64:
    PCIe devices with 32-bit DMA mask AND no IOMMU
    USB controllers with limited DMA range
    Platform DMA without IOMMU
    
  swiotlb_force=force: kernel boot parameter to always use swiotlb
    (for testing, for edge cases in secure boot environments)

CMA (Contiguous Memory Allocator): separate mechanism for large physical buffers
  dma_alloc_from_contiguous(dev, count, align, no_warn):
    Allocates from CMA region (movable pages, but CMA reserves physical area)
    Used when device needs large contiguous buffers (video, camera)
    See Category_11/08 for CMA details
```

---

## 5. dma_pool: Small DMA Buffers

```
Problem: dma_alloc_coherent minimum allocation = 1 page (4KB)
         Many descriptors are 16–64 bytes each (huge waste!)
         Creating 256 DMA allocations of 32 bytes each = 256 × 4KB = 1MB wasted!

dma_pool: slab-like allocator for small DMA-coherent objects
  All objects in one dma_pool are:
    - Same size (object_size)
    - Same alignment
    - Guaranteed <= PAGE_SIZE
    - Contiguous within one page (but pages may be scattered)
    - Cache coherent

struct dma_pool *dma_pool_create(const char *name, struct device *dev,
                                  size_t size, size_t align, size_t allocation):
    size: object size
    align: alignment (must be power of 2)
    allocation: if non-zero: objects must not cross this boundary
    
    Creates a pool of 4KB pages, each subdivided into aligned objects
    First page allocated lazily on first dma_pool_alloc

void *dma_pool_alloc(struct dma_pool *pool, gfp_t gfp, dma_addr_t *handle):
    Find a pool page with a free slot
    If no free slots: dma_alloc_coherent(dev, PAGE_SIZE) → new pool page
    Return: virtual address + dma_handle for the object

void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr):
    Mark slot as free in pool page bitmap
    If pool page is all free: keep it or return to system (based on pool policy)

dma_pool_destroy(struct dma_pool *pool):
    Free all pool pages: dma_free_coherent for each page
    
Typical use: network ring descriptors, SCSI command structures, USB transfer descriptors
Example:
    pool = dma_pool_create("rx_desc", dev, sizeof(struct rx_desc), 64, 0);
    desc = dma_pool_alloc(pool, GFP_KERNEL, &desc_dma);
    // desc_dma: physical address for DMA ring programming
```

---

## 6. Interview Questions & Answers

**Q1: What's the difference between dma_alloc_coherent() and dma_map_single()?**

Both provide DMA-capable addresses, but for different usage patterns:

**dma_alloc_coherent()**: allocates a NEW buffer specifically for DMA. The buffer is always coherent (either hardware-coherent, or mapped as non-cacheable). Both CPU and device can access it at any time without synchronization. Best for: descriptor rings, persistent command buffers, DMA regions that are read AND written by both CPU and device simultaneously.

**dma_map_single()**: takes an EXISTING kernel buffer (already allocated with kmalloc/alloc_pages) and prepares it for a single DMA transfer. Requires explicit direction (TO_DEVICE or FROM_DEVICE). On non-coherent systems, performs cache maintenance (flush or invalidate). Must call `dma_unmap_single()` after transfer completes. Best for: one-shot transfers of data that's already in normal (cached) kernel memory.

The key difference: coherent buffers can be accessed any time by both sides; streaming (mapped) buffers must follow map→transfer→unmap discipline and shouldn't be accessed by CPU between map and unmap.

**Q2: Why does ARM64 need swiotlb if it supports 64-bit DMA?**

Not all devices on ARM64 systems support 64-bit DMA addressing. Many legacy IP blocks (integrated into SoCs), older PCIe peripherals, and USB controllers have 32-bit DMA address registers and can only address memory below 4GB. 

When an ARM64 system has more than 4GB of RAM and the kernel allocates a buffer at a high physical address for such a device, the device cannot reach that buffer. swiotlb allocates a bounce buffer in the low 4GB region, copies data to/from it, and gives the device the low-address buffer.

With an IOMMU/SMMU present (common on modern ARM64 servers), swiotlb is usually not needed — the IOMMU maps a 32-bit IOVA to the actual high physical address, and the device accesses memory through IOVA→PA translation performed by the SMMU hardware.

---

## 7. Quick Reference

| DMA API | Use Case | Cache Coherent? | Physically Contiguous? |
|---|---|---|---|
| dma_alloc_coherent() | Persistent DMA buffer | YES (always) | YES |
| dma_map_single() | One-shot buffer | After map/unmap | Must be (for direct) |
| dma_map_sg() | Scatter-gather | After map/unmap | Per-entry |
| dma_pool_alloc() | Small objects (<PAGE) | YES | YES (within PAGE) |
| dma_alloc_from_contiguous() | Large contiguous | YES | YES (CMA) |

| ARM64 DMA Coherency | Scenario |
|---|---|
| Fully coherent | ACE/ACE-Lite DMA master, IOMMU with coherent domain |
| Partially coherent | Non-coherent device + SMMU with outer-shareable |
| Non-coherent | No IOMMU, non-coherent DMA: need cache flush/invalidate |
