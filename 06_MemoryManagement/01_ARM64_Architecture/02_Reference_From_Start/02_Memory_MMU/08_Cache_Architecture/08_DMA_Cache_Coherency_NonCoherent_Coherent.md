# DMA Cache Coherency: Non-Coherent and Coherent DMA Deep Dive

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. The DMA Cache Problem

```
DMA (Direct Memory Access): hardware device reads/writes memory directly
  bypassing the CPU, accessing physical memory (PA) via the system bus.

Problem: CPU caches are transparent to the CPU but INVISIBLE to DMA devices.
  CPU writes to memory:
    → Data goes to CPU L1/L2 cache (DRAM may be STALE)
    DMA reads from PA: reads DRAM directly → gets STALE DATA!
    
  DMA writes to memory:
    → Data written to DRAM (device writes)
    CPU reads from VA: hits L1/L2 cache → gets OLD DATA (cache has pre-DMA content)!
    
This is the DMA cache coherency problem.

Two solutions:
  1. Hardware coherent DMA: device is connected to the CPU's coherency fabric
     (ACE/CCI) → device sees CPU cache via snooping
     No software cache maintenance needed!
     
  2. Software-managed DMA: device is NOT coherent
     Software must explicitly flush/invalidate caches before/after DMA
     Most embedded/mobile SoCs: DMA controllers are NOT coherent
     Exceptions: PCIe devices with hardware coherency support

ARM64 SoC reality:
  Qualcomm Snapdragon: DMA controllers (USB, UFS, display, codec) are NOT coherent
  Apple M-series: some DMA (USB, NVMe) may be coherent via hardware
  Server SoCs (Neoverse): PCIe devices via SMMU may achieve coherency
```

---

## 2. Linux DMA API: Cache Maintenance Details

```
Linux DMA API (include/linux/dma-mapping.h):

dma_map_single(dev, cpu_addr, size, direction):
  Prepares a buffer for DMA access by the device.
  
  DMA_TO_DEVICE (CPU → Device):
    CPU wrote data to buffer; device will READ from buffer
    Action: Clean caches (DC CVAC) for entire buffer range
    Effect: dirty cache lines pushed to DRAM → device reads current data from DRAM
    After dma_map: CPU must NOT write to buffer until dma_unmap!
    
  DMA_FROM_DEVICE (Device → CPU):
    Device will WRITE to buffer; CPU will READ after DMA completes
    Action: Invalidate caches (DC IVAC) for entire buffer range
    Effect: CPU's stale cache copies discarded → CPU reads fresh DRAM data
    After dma_map: CPU must NOT read from buffer until dma_unmap!
    Warning: if buffer has dirty CPU data → DC IVAC discards it (data lost!)
             Always clean before invalidating for DMA_FROM_DEVICE if unsure:
             Use DMA_BIDIRECTIONAL instead to be safe
             
  DMA_BIDIRECTIONAL:
    Both directions: device reads AND writes
    Action: DC CIVAC (clean + invalidate) for entire buffer range
    Effect: dirty CPU data pushed to DRAM; CPU cache invalidated
    After DMA: CPU and device both see each other's writes
    
dma_unmap_single(dev, dma_addr, size, direction):
  Marks DMA complete; CPU can access the buffer again.
  
  DMA_FROM_DEVICE: MUST invalidate again on some platforms
    (some CPUs speculatively prefetch during DMA, polluting cache with old data)
    Linux has_dma_ops → calls arch_sync_dma_for_cpu() which may DC IVAC again
    
  DMA_TO_DEVICE: usually no action (cache already clean from dma_map)

dma_sync_single_for_cpu(dev, dma_addr, size, direction):
  Use when: CPU needs to temporarily access a DMA-mapped buffer mid-transfer
  Invalidates cache (DMA_FROM_DEVICE) or no-op (DMA_TO_DEVICE)
  
dma_sync_single_for_device(dev, dma_addr, size, direction):
  CPU done; device takes over again
  Cleans cache (DMA_TO_DEVICE) or no-op (DMA_FROM_DEVICE)
```

---

## 3. dma_alloc_coherent: Coherent (Streaming vs Persistent)

```
dma_alloc_coherent(dev, size, dma_handle, GFP_KERNEL):
  Allocates a buffer that is PERMANENTLY coherent between CPU and device.
  No cache maintenance needed for any access!
  
  How it achieves coherency:
    On non-coherent ARM SoC: map the buffer as NC Normal memory (MAIR Attr2)
    CPU accesses: go directly to DRAM (no L1/L2 caching)
    Device accesses: go directly to DRAM
    Both always see the same DRAM content → permanently coherent
    
    Cost: CPU accesses are DRAM-speed (no cache benefit) = slow!
    Typical use: small control structures (DMA descriptors, ring buffers)
    NOT suitable: large data buffers where CPU performance matters
    
  On coherent SoC (hardware coherency via SMMU+ACE):
    Buffer mapped as Normal WB (cached)
    Hardware ensures coherency automatically
    CPU gets full cache performance + device coherency!
    
  dma_free_coherent(dev, size, cpu_addr, dma_handle):
    Frees the coherent buffer, unmaps from device address space
    
  Buffer size limit:
    Large coherent buffers (> few MB) may fail: need contiguous physical memory
    Solution: use DMA pools (dma_pool_create) or CMA (Contiguous Memory Allocator)

Comparison: streaming vs coherent DMA
  
  Streaming DMA (dma_map_single):
    Normal WB-cached memory + explicit sync operations
    Fast CPU access (fully cached)
    Requires dma_map/dma_unmap around each DMA transfer
    Good for: large data buffers (network packets, disk I/O)
    
  Coherent DMA (dma_alloc_coherent):
    NC Normal memory (or hardware coherent)
    On non-coherent SoC: slow CPU access (no cache)
    No sync operations needed
    Good for: small, frequently accessed control structures (DMA descriptors)
    
  Rule of thumb:
    Small, frequently CPU-written structures: coherent DMA
    Large data I/O buffers: streaming DMA (map/unmap per transfer)
```

---

## 4. Bounce Buffers and SWIOTLB

```
Bounce buffer: a temporary staging buffer used when DMA constraints can't be met.

Problem scenario:
  Device has a 32-bit DMA address limit (DMA_BIT_MASK(32))
  But the buffer is allocated at PA > 4GB (64-bit ARM SoC with > 4GB RAM)
  Device cannot address PA directly!

SWIOTLB (Software IO TLB) bounce buffer mechanism:
  Linux maintains a pool of "bounce buffers" in low memory (< 4GB PA)
  
  dma_map_single() detects: buffer PA > device DMA mask
    → Allocate a bounce buffer in low memory
    → For DMA_TO_DEVICE: memcpy from original buffer to bounce buffer
    → Return bounce buffer DMA address to device
    
  DMA transfer: device reads from bounce buffer (low memory, reachable)
  
  dma_unmap_single():
    For DMA_FROM_DEVICE: memcpy from bounce buffer back to original buffer
    
  Overhead: extra memcpy for every DMA transfer
  Performance: bounce buffer effectively halves DMA bandwidth (memcpy cost)
  
  SWIOTLB size:
    Default: 64 MB (max bounce buffer pool)
    Configurable: swiotlb=256 (256 MB) in kernel cmdline
    If pool exhausted: GFP_NOIO allocation fails → DMA failure!
    Common issue: embedded SoC with 32-bit DMA + > 4GB RAM = SWIOTLB pressure

IOMMU-based DMA remapping (SMMU):
  SMMU provides I/O virtual addresses (IOVA) separate from PA
  Device uses IOVA → SMMU translates to PA
  Eliminates bounce buffer need: any PA accessible via SMMU mapping!
  AMD/Intel IOMMU (VT-d): same concept for PCIe devices
  ARM SMMU: SMMUv3 for ARM64 servers
```

---

## 5. Cache Line Alignment for DMA

```
DMA cache line alignment requirements:

Critical: DMA buffer must be cache-line aligned (64 bytes on ARM64).
If buffer is NOT cache-line aligned:
  DMA_FROM_DEVICE: device writes to PART of a cache line
  That cache line also contains OTHER data (neighboring struct fields)
  DC IVAC on that cache line: discards ALL data in the line
  → OTHER data (not part of DMA buffer) is LOST!
  → Memory corruption: neighboring fields zeroed or corrupted
  
Linux guarantees:
  kmalloc(): returns cache-line-aligned memory for allocations ≥ cache_line_size
  dma_alloc_coherent(): always cache-line-aligned
  
  For streaming DMA: application must ensure alignment:
    DEFINE_DMA_UNMAP_ADDR(mapping):
      Stores the DMA address for safe unmap
    Use: kmalloc(size, GFP_DMA) for DMA-safe memory allocation
    Or: dma_pool_alloc() for small DMA allocations

False sharing with DMA:
  If a CPU-local struct shares a cache line with DMA buffer:
    DMA writes → DC IVAC → entire cache line invalidated including CPU data
    CPU's local data (NOT part of DMA buffer) appears corrupted!
    
  Prevention: 
    __cacheline_aligned: ensure DMA buffers start at cache line boundary
    Pad structs to avoid DMA-active fields sharing lines with CPU-only fields
    Use separate DMA descriptor structs, not embedded in larger structs

struct sk_buff (network packet buffer):
  .data field: always cache-line aligned for DMA
  DMA header (hardware descriptor): separate allocation, cache-line aligned
  Linux networking: careful separation of DMA and non-DMA fields in sk_buff
```

---

## 6. Interview Questions & Answers

**Q1: Explain the difference between dma_map_single and dma_alloc_coherent. When should you use each?**

`dma_map_single()` is for **streaming DMA** — it takes an existing kernel buffer (normally Write-Back cached memory), performs the necessary cache maintenance (`DC CVAC` for `DMA_TO_DEVICE`, `DC IVAC` for `DMA_FROM_DEVICE`), and returns a DMA address for the device. The buffer remains in the CPU's cache for fast CPU access; you just sync caches at the boundaries of DMA transfers. This is efficient for large data I/O (network packets, disk blocks) because CPU access is fully cached between DMA operations.

`dma_alloc_coherent()` allocates **permanently coherent memory** — typically mapped as Non-Cacheable Normal on non-coherent ARM SoCs. Every CPU access goes directly to DRAM (bypassing cache), and every device DMA access also goes to DRAM. Since both the CPU and device always see DRAM directly, there's no stale cache issue and no sync operations are ever needed. The trade-off is that CPU access is DRAM-speed, not cached. This makes it ideal for **small, frequently accessed control structures** like DMA descriptors, ring buffers, and status words — structures the CPU accesses occasionally but the device accesses very frequently.

Rule: large data buffers → streaming DMA; small control structures → coherent DMA.

---

## 7. Quick Reference

| DMA Direction | dma_map action | Cache maintenance |
|---|---|---|
| DMA_TO_DEVICE | CPU→Device | DC CVAC: clean to PoC |
| DMA_FROM_DEVICE | Device→CPU | DC IVAC: invalidate at PoC |
| DMA_BIDIRECTIONAL | Both | DC CIVAC: clean+invalidate |

| DMA Type | Memory | CPU Access Speed | Sync Needed? | Best For |
|---|---|---|---|---|
| Streaming | Normal WB | Cache speed (fast) | Yes (map/unmap) | Large data (net, disk) |
| Coherent | NC Normal | DRAM speed (slow) | No | Small descriptors, rings |

| Issue | Cause | Fix |
|---|---|---|
| Device reads stale data | DMA_TO_DEVICE without map | dma_map_single before DMA |
| CPU reads stale data | DMA_FROM_DEVICE without unmap | dma_unmap or sync_for_cpu |
| Memory corruption | DMA buffer not cache-line aligned | Align to 64 bytes |
| DMA bounce | PA exceeds device DMA mask | SWIOTLB or IOMMU |
