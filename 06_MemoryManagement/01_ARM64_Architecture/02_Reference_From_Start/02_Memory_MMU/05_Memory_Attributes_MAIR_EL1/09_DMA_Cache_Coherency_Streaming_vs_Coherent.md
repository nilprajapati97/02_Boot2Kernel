# DMA Cache Coherency: Streaming vs Coherent DMA

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. The DMA Coherency Problem

```
DMA (Direct Memory Access) engines access DRAM independently of the CPU:

CPU view: accesses memory through L1/L2/L3 caches
DMA engine view: accesses DRAM directly (bypasses CPU caches on non-coherent SoCs)

The incoherency:
  CPU writes to buffer (data in L1/L2 dirty cache, NOT yet in DRAM)
  DMA engine reads from DRAM → sees OLD data (stale)
  
  OR:
  DMA engine writes to DRAM (new data)
  CPU reads from buffer → L1 cache hit → OLD data (stale CPU cache)

Two solutions:
  1. SOFTWARE coherency: explicit cache flushes (streaming DMA)
  2. HARDWARE coherency: DMA engine participates in snoop protocol (coherent DMA)
```

---

## 2. Streaming DMA (Software-Managed Coherency)

```
Used on SoCs where DMA engine is NOT connected to CPU coherency fabric.
CPU must explicitly flush/invalidate cache before/after DMA operations.

Linux DMA API for streaming:
  dma_map_single(dev, cpu_addr, size, direction)
    - Maps a single CPU virtual address for DMA
    - Returns DMA bus address (for device programming)
    - Performs cache maintenance based on direction:
      DMA_TO_DEVICE: DC CIVAC (flush+invalidate) → DRAM has CPU's current data
      DMA_FROM_DEVICE: DC IVAC (invalidate) → remove stale CPU cache lines
      DMA_BIDIRECTIONAL: DC CIVAC → both

  dma_unmap_single(dev, dma_addr, size, direction)
    - Unmaps the DMA address
    - May perform additional cache maintenance:
      DMA_FROM_DEVICE: DC IVAC again (ensure CPU reads fresh DMA data)

  dma_sync_single_for_device(dev, dma_addr, size, DMA_TO_DEVICE)
    - Use if you need to re-sync (after CPU modifies buffer during DMA lifecycle)
    - Equivalent to DC CIVAC

  dma_sync_single_for_cpu(dev, dma_addr, size, DMA_FROM_DEVICE)
    - Call after DMA completes, before CPU reads result
    - Equivalent to DC IVAC

Timeline (DMA TX example):
  cpu_buf = kmalloc(size, GFP_KERNEL);     // Allocates Normal WB buffer
  // ... fill cpu_buf with data ...
  dma_addr = dma_map_single(dev, cpu_buf, size, DMA_TO_DEVICE);
  //   → DC CIVAC: CPU's dirty cache lines flushed to DRAM
  //   → DMA engine now sees CPU's data in DRAM
  program_dma(dma_addr, size);              // Set up DMA descriptor
  start_dma();                               // Kick DMA
  wait_for_completion(&dma_done);            // Wait for IRQ
  dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE);
  // No additional cache work needed for TO_DEVICE (DMA read-only)
```

---

## 3. Coherent DMA (Hardware-Managed Coherency)

```
Used on SoCs where DMA engine is connected to the coherency interconnect
(e.g., ARM CCI/CMN with DMA engine as an agent on the interconnect bus).

On coherent SoCs:
  DMA engine is part of Inner Shareable or Outer Shareable domain
  CPU's cache writes are snooped by the interconnect
  DMA reads get the latest CPU data via cache snoop (hit → returns cached data)
  DMA writes are propagated to CPU caches (invalidate on CPU snoop)
  
  No explicit cache flushes needed!

Linux API for coherent DMA:
  dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL)
    On coherent SoC: allocates Normal WB memory, maps for both CPU and DMA
    On non-coherent SoC: allocates Normal NC memory (bypasses CPU cache)
    Returns: CPU virtual address (for CPU access) + DMA bus address (for device)

  dma_free_coherent(dev, size, cpu_addr, dma_addr)
    Frees the coherent buffer

Detection at runtime:
  dev_is_dma_coherent(dev) → returns true on coherent SoC
  Controlled by: device tree "dma-coherent" property
                 or platform code setting dev->dma_coherent = true

Coherent buffer memory type:
  Non-coherent SoC: MT_NORMAL_NC (0x44) → both CPU and DMA access DRAM directly
  Coherent SoC: MT_NORMAL (0xFF) → CPU uses cache, DMA is snooped → coherent
```

---

## 4. Bounce Buffers: Handling Address Limitations

```
Some DMA devices have limited address range:
  Legacy 32-bit PCI DMA: can only access PA < 4 GB
  Some IoT SoC DMA: can only access first 256 MB
  
  If buffer is above the DMA device's range → DMA cannot access it!

Solution: Bounce Buffer

  Allocate a secondary "bounce buffer" within the DMA device's accessible range
  
  For DMA_TO_DEVICE (CPU buffer above range, DMA reads):
    1. CPU fills its buffer at high PA (above 4 GB)
    2. Linux SWIOTLB memcpy: copies CPU buffer → bounce buffer (within 4 GB)
    3. DC CIVAC: flush bounce buffer to DRAM
    4. Program DMA with bounce buffer PA
    5. DMA reads from bounce buffer → correct data ✓

  For DMA_FROM_DEVICE (DMA writes, CPU reads):
    1. Program DMA with bounce buffer PA
    2. DMA writes to bounce buffer (within 4 GB)
    3. DC IVAC: invalidate CPU cache for bounce buffer
    4. Linux SWIOTLB memcpy: copies bounce buffer → CPU buffer (at high PA)
    5. CPU reads from its buffer → correct DMA data ✓

  SWIOTLB (Software IO Translation Lookaside Buffer):
    Linux subsystem that manages bounce buffer pool
    Configured with swiotlb=N (N pages of bounce buffer space)
    Enabled automatically for devices with DMA mask < physical RAM size
    
  Memory overhead: bounce buffer = double-copy latency + extra DRAM
  → Only used when device cannot reach physical buffer directly
  → Modern SoCs with IOMMU: IOVA remapping eliminates need for bounce buffers
    (IOMMU maps high-PA buffer to within device's DMA range)
```

---

## 5. DMA Direction and Cache Operations

```
DMA Directions and Required Cache Operations:

DMA_TO_DEVICE (CPU writes, DMA reads):
  Before DMA:  DC CIVAC (Clean+Invalidate to PoC)
    → Ensures CPU's dirty cache data is in DRAM for DMA to read
  After DMA:   NONE (DMA only read, didn't modify DRAM)

DMA_FROM_DEVICE (DMA writes, CPU reads):
  Before DMA:  DC IVAC (Invalidate to PoC) — or nothing if already invalidated
    → Ensure any stale CPU cache lines are gone before DMA writes DRAM
    (Prevent CPU's stale write from being written back AFTER DMA wrote DRAM)
  After DMA:   DC IVAC (Invalidate to PoC)
    → Remove any stale CPU cache lines so CPU reads fresh DRAM data

DMA_BIDIRECTIONAL (DMA reads AND writes):
  Before DMA:  DC CIVAC (both clean and invalidate)
  After DMA:   DC IVAC (invalidate so CPU reads fresh DMA-written data)

Key insight:
  The BEFORE and AFTER operations differ:
  Before: must CLEAN to ensure CPU writes reach DRAM
  After: must INVALIDATE to ensure CPU reads come from DRAM (not stale cache)
  CIVAC (before) handles both: cleans AND invalidates
  
  Some implementations use CIVAC both before AND after for simplicity
  (slightly conservative but always correct)
```

---

## 6. Platform Differences: Coherent vs Non-Coherent SoCs

```
Mobile (Qualcomm Snapdragon, Samsung Exynos):
  Modern: Coherent DMA for major peripherals (GPU, image processor, video)
    via AXI interconnect with cache coherency
  Legacy: Non-coherent (older SoCs require explicit cache ops for peripheral DMAs)
  
  Camera controller: May be non-coherent → use dma_sync*() for each frame
  GPU: Coherent on modern SoCs → dma_alloc_coherent() returns WB memory
  WiFi DMA: Often non-coherent → SWIOTLB + dma_sync*()

Server (ARM Neoverse, AWS Graviton, Ampere Altra):
  Fully coherent: All DMA engines connected via CMN-700 or similar
  SMMU provides I/O coherency for PCIe devices
  dma_alloc_coherent() returns Normal WB memory (no NC needed)
  No explicit cache flushes needed in drivers

Embedded (Raspberry Pi, Rockchip, Allwinner):
  Non-coherent or partially coherent
  Many peripherals (USB, ETH, display) require explicit DMA cache ops
  SWIOTLB often enabled for devices with 30-bit or 32-bit DMA masks
```

---

## 7. Interview Questions & Answers

**Q1: What is the difference between streaming DMA and coherent DMA in Linux?**

**Streaming DMA** uses explicitly mapped/unmapped buffers with `dma_map_single/dma_unmap_single`. The CPU maps a normal (cached) buffer for DMA, performs the DMA operation, then unmaps it. Cache flushes (`DC CIVAC` or `DC IVAC`) are performed at map/unmap time or via `dma_sync_*` calls. The buffer can be reused after unmapping. Useful for one-shot DMA operations where the buffer lives in the kernel's normal heap.

**Coherent DMA** (`dma_alloc_coherent`) allocates a buffer that is **always coherent** between CPU and DMA engine, at all times. On non-coherent SoCs, this means allocating Non-Cacheable memory (bypasses L1/L2 so no flush ever needed). On coherent SoCs, it allocates WB cached memory (DMA is part of the snoop domain). No explicit cache maintenance ever required — CPU and DMA always see the same data. The trade-off: coherent memory is typically slower for CPU access on non-coherent SoCs (NC = every access hits DRAM) but has zero synchronization overhead.

---

## 8. Quick Reference

| Scenario | Memory Type | Before DMA | After DMA | API |
|---|---|---|---|---|
| CPU→DMA, non-coherent | Normal NC or WB+flush | DC CIVAC | None | `dma_map_single(DMA_TO_DEVICE)` |
| DMA→CPU, non-coherent | Normal NC or WB+flush | DC IVAC | DC IVAC | `dma_map_single(DMA_FROM_DEVICE)` |
| Both, non-coherent | Normal WB+flush | DC CIVAC | DC IVAC | `dma_map_single(BIDIRECTIONAL)` |
| Always coherent, non-coherent SoC | Normal NC | None | None | `dma_alloc_coherent()` |
| Always coherent, HW coherent SoC | Normal WB | None | None | `dma_alloc_coherent()` |
| Device address limit | Bounce buffer | Copy+flush | Copy+inval | SWIOTLB automatic |
