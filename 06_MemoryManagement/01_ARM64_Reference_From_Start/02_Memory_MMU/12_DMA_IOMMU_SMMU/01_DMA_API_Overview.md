# DMA API Overview: dma_map_single, dma_map_sg, Directions

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
DMA (Direct Memory Access): hardware transfers data without CPU involvement
  CPU: processes data (L1/L2 cache → registers → L1/L2 cache)
  DMA engine: accesses DRAM directly (physical address bus)
  
  ARM64 DMA Actors:
    DMA masters: devices that INITIATE DMA (PCIe endpoints, eMMC, USB)
    DMA target:  system DRAM (the recipient/source of DMA transfers)
    IOMMU/SMMU:  optional intermediary (translates device VA → physical PA)
  
  DMA Address vs CPU Virtual Address:
    CPU accesses memory via: virtual address → MMU → physical address
    Device accesses memory via: I/O virtual address (IOVA) → SMMU → physical address
                               OR physical address directly (no SMMU)
    
    These may differ when SMMU is present!
    The DMA address returned by the DMA API is what the DEVICE uses:
      Without SMMU: dma_address = physical address (maybe minus offset)
      With SMMU: dma_address = IOVA (I/O virtual address, device-visible)
  
  Linux DMA API:
    Single point of abstraction for all platforms
    Driver uses DMA API → gets dma_addr_t (opaque device address)
    Driver programs device registers with dma_addr_t
    Driver doesn't need to know: is SMMU present? is RAM low enough? swiotlb?
    The DMA API handles all translation, coherency, and addressing

DMA API categories:
  1. Coherent (persistent) DMA: dma_alloc_coherent()
     Always cache-coherent, persistent buffer, both sides can access anytime
  
  2. Streaming (one-shot) DMA: dma_map_single(), dma_map_sg()
     Existing buffer, map before transfer, unmap after
     Must respect transfer direction
  
  3. Pool allocation: dma_pool_alloc()
     Small objects from a pre-allocated DMA-coherent pool
```

---

## 2. DMA Setup Prerequisites

```
Before using DMA API: device must be set up

1. Set DMA mask (what physical addresses device can reach):
   dma_set_mask(dev, DMA_BIT_MASK(64)):   device can reach all 64-bit PA
   dma_set_mask(dev, DMA_BIT_MASK(32)):   device limited to 4GB
   dma_set_mask(dev, DMA_BIT_MASK(30)):   device limited to 1GB
   
   Combined mask (also sets coherent DMA mask):
   dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)):

2. Check platform capability:
   dma_addressing_limited(dev): returns true if 32-bit DMA limited
   
   ARM64 example (from DTS):
   dma-ranges = <0x0 0x0 0x0 0x0 0x80000000 0x0>; // device can reach first 2GB
   
3. Enable DMA (PCIe):
   pci_set_master(pdev): enables bus mastering (DMA from PCIe device)

DMA direction enum:
  DMA_TO_DEVICE     = 1:  CPU writes buffer, device reads (e.g., TX ring)
  DMA_FROM_DEVICE   = 2:  device writes buffer, CPU reads (e.g., RX ring)
  DMA_BIDIRECTIONAL = 3:  both (use for ring buffers that do both)
  DMA_NONE          = 4:  no DMA (used for checks)
  
  Importance of correct direction:
    DMA_TO_DEVICE: cache flush (clean) before DMA (so device reads up-to-date data)
    DMA_FROM_DEVICE: cache invalidate after DMA (so CPU reads device-written data)
    DMA_BIDIRECTIONAL: flush + invalidate (conservative, correct, but more expensive)
    
    WRONG direction: data corruption! CPU reads stale cached values.
```

---

## 3. dma_map_single: Single Buffer DMA

```c
/* include/linux/dma-mapping.h */

dma_addr_t dma_map_single(struct device *dev, void *ptr,
                           size_t size, enum dma_data_direction dir)
{
    // ptr: kernel virtual address of buffer (kmalloc'd, on stack, etc.)
    // size: buffer size in bytes
    // dir: DMA_TO_DEVICE / DMA_FROM_DEVICE / DMA_BIDIRECTIONAL
    
    // Returns: dma_addr_t (opaque device address)
    //   Without IOMMU: physical address (maybe with swiotlb redirect)
    //   With IOMMU: IOVA
    
    // Error: DMA_MAPPING_ERROR (check with dma_mapping_error(dev, dma_addr))
    
    // Must call dma_unmap_single() when transfer is done!
}

// Check for mapping error (MUST check!):
if (dma_mapping_error(dev, dma_addr)) {
    // Handle failure: free resources, return error
    kfree(buf);
    return -ENOMEM;
}

void dma_unmap_single(struct device *dev, dma_addr_t dma_addr,
                       size_t size, enum dma_data_direction dir)
{
    // Reverses the mapping
    // On non-coherent systems: cache operations if needed
    // With IOMMU: removes IOVA → PA mapping from SMMU page tables
    // Frees swiotlb bounce buffer if used
}

// If CPU needs to access the buffer BETWEEN map and unmap:
// (Don't do this if possible, but sometimes needed for partial writes)
void dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma,
                               size_t size, enum dma_data_direction dir);
// After: CPU can safely read/modify the buffer

void dma_sync_single_for_device(struct device *dev, dma_addr_t dma,
                                  size_t size, enum dma_data_direction dir);
// After: device can safely DMA the buffer again (CPU done with it)
```

---

## 4. dma_map_sg: Scatter-Gather DMA

```c
// Scatter-gather: device can handle non-contiguous physical memory
// via a list of (address, length) pairs in its DMA descriptor

struct scatterlist {
    unsigned long    page_link;   // struct page* + flags
    unsigned int     offset;       // offset within page
    unsigned int     length;       // length of this segment
    dma_addr_t       dma_address;  // DMA address after mapping
    unsigned int     dma_length;   // DMA length after mapping (may merge entries)
};

// Allocate scatter-gather list:
int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp);
// or: sg_alloc_table_from_pages() for vmalloc'd memory

// Map for DMA:
int dma_map_sg(struct device *dev, struct scatterlist *sg,
               int nents, enum dma_data_direction dir)
{
    // For each sg entry:
    //   Map page + offset → DMA address
    //   With IOMMU: create IOVA → PA mappings
    //   Without IOMMU: physical address
    // Returns: number of mapped entries (may be < nents if merged!)
    //          Returns 0 on error
    
    // IOMMU optimization: adjacent entries with contiguous PA
    //   can be merged into one IOVA range → fewer sg entries for device
}

// Iterate mapped entries:
for_each_sg(sgl, sg, mapped_nents, i) {
    dma_addr_t dma = sg_dma_address(sg);   // device DMA address
    unsigned int len = sg_dma_len(sg);      // segment length
    
    // Program device DMA descriptor:
    desc[i].addr = cpu_to_le64(dma);
    desc[i].len  = cpu_to_le32(len);
}

// After transfer:
dma_unmap_sg(dev, sg, nents, dir);

// DMA-API mapping rules (MUST follow):
// 1. Don't modify buffer between map and unmap (without sync)
// 2. Don't read buffer after dma_map_sg, before dma_unmap_sg
//    (without dma_sync_sg_for_cpu)
// 3. Call unmap even if transfer failed (to release IOVA/bounce)
// 4. Check return value of dma_map_sg (0 = error)
```

---

## 5. ARM64 DMA Coherency

```
ARM64 DMA coherency model:
  Fully coherent (most ARM64 SoCs):
    DMA master connected via ACE-Lite / CCI / CMN (Coherent Mesh Network)
    All DMA reads/writes visible through CPU cache coherency protocol
    CPU cache and device see same data automatically
    No software cache operations needed
    dma_map_single(): just returns physical address, no cache ops
    
  Non-coherent (some peripherals):
    Device NOT on coherency interconnect
    DMA reads: may get stale cached CPU data
    DMA writes: CPU may read stale cached data
    Software must maintain coherency:
      dma_map_single(DMA_TO_DEVICE): DC CIVAC (clean+invalidate all cache lines)
      dma_map_single(DMA_FROM_DEVICE): DC IVAC (invalidate cache lines)
      dma_unmap_single(): reverse operation
    
    ARM64 cache instructions for DMA:
      DC CIVAC, Xn: Clean+Invalidate to Point of Coherency (DRAM)
                    Ensures device sees CPU's writes AND CPU won't read stale after device writes
      DC CVAC, Xn:  Clean to PoC (flush dirty lines to DRAM)
      DC IVAC, Xn:  Invalidate to PoC (throw away cached lines)
      DSB SY:       Wait for all cache ops to complete

  Coherency check at device registration:
    of_dma_is_coherent(dev->of_node): checks DT "dma-coherent" property
    If coherent: dev->archdata.dma_coherent = true
    dma_map_single(): skips cache ops if device is DMA-coherent
```

---

## 6. Interview Questions & Answers

**Q1: Why must you call dma_unmap_single() even if the DMA transfer failed?**

`dma_map_single()` acquires resources that must be released regardless of transfer outcome:

1. **IOVA allocation**: if an IOMMU is present, `dma_map_single()` allocated an IOVA (I/O Virtual Address) and created an SMMU page table entry (IOVA → PA). Not unmapping leaks the IOVA space and leaves stale SMMU mappings. IOVA space is finite — leaking it causes future allocations to fail.

2. **swiotlb bounce buffer**: if a bounce buffer was used (32-bit device, high physical address), `dma_map_single()` reserved a slot in the swiotlb pool. Not releasing it leaks the bounce buffer, eventually exhausting all swiotlb slots.

3. **Cache state**: the buffer's cache lines were put in a specific state (flushed/invalidated). Not completing the map/unmap cycle may leave incorrect assumptions about cache state.

Rule: **map + unmap are paired operations, like malloc + free**. Always call unmap in error paths.

**Q2: What is DMA_MAPPING_ERROR and how do you detect it?**

`DMA_MAPPING_ERROR` is a sentinel value returned by `dma_map_single()` on failure. On most architectures, it's `(~(dma_addr_t)0)` (all bits set), but the exact value is platform-specific.

Never compare directly with `DMA_MAPPING_ERROR`; always use:
```c
dma_addr = dma_map_single(dev, ptr, size, dir);
if (dma_mapping_error(dev, dma_addr)) {
    /* handle failure */
}
```

Failure causes:
- IOMMU IOVA space exhausted (too many mappings not unmapped)
- Physical address doesn't fit in device's DMA mask (but this usually uses swiotlb instead)
- swiotlb buffer full (too many 32-bit DMA mappings outstanding)

---

## 7. Quick Reference

| DMA Operation | Function | Notes |
|---|---|---|
| Map single buffer | dma_map_single() | Returns dma_addr_t |
| Unmap single | dma_unmap_single() | MUST be called |
| Map scatter-gather | dma_map_sg() | Returns mapped count |
| Unmap sg | dma_unmap_sg() | Use original nents |
| Sync for CPU access | dma_sync_single_for_cpu() | Between map/unmap |
| Sync for device | dma_sync_single_for_device() | CPU done, device resumes |
| Check mapping error | dma_mapping_error() | Check return of map |
| Set DMA mask | dma_set_mask_and_coherent() | Before first DMA |
