# Coherent vs Streaming DMA: ARM64 Deep Dive

**Category**: DMA, IOMMU, and SMMU  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Two fundamental DMA patterns in Linux:

1. Coherent DMA (a.k.a. consistent DMA):
   - Memory always CPU-accessible AND device-accessible simultaneously
   - No explicit sync calls needed between CPU and device access
   - Memory is either uncacheable (no cache coherency problem) or
     on a coherent interconnect (hardware maintains coherency)
   - Lifetime: allocated once, used many times (persistent)
   - APIs: dma_alloc_coherent(), dma_free_coherent()
   - Example: DMA descriptor rings, shared control structures

2. Streaming DMA (a.k.a. transactional DMA):
   - Temporary mapping for a single DMA transaction
   - Explicit sync calls required between CPU and device ownership
   - Memory may be normal cacheable pages (driver's buffer, user pages)
   - Lifetime: map → DMA operation → unmap → reuse buffer
   - APIs: dma_map_single(), dma_map_sg(), dma_unmap_*()
   - Example: network packet buffers, storage I/O buffers

The critical difference:
  Coherent:  no sync discipline required (hardware handles it)
  Streaming: MUST follow map/sync/unmap protocol (SW handles coherency)
```

---

## 2. ARM64 Coherency Mechanisms

```
ARM64 platform coherency (affects which approach is needed):

CASE 1: Coherent interconnect (most modern ARM64 servers, phones)
  Bus: ARM CMN-700 (Coherent Mesh Network), CCI-550, CCN-512
  Protocol: AMBA CHI or ACE (AXI Coherency Extensions)
  Coherency type: hardware snooping (device DMA snoops CPU caches)
  
  Device DMA read: 
    Request goes through interconnect
    Interconnect checks if data is dirty in any CPU cache
    If dirty: snoop CPU cache → forward data to device
    → Device always sees latest data, no CPU flush needed
  
  Device DMA write:
    Write goes through interconnect
    Interconnect invalidates matching CPU cache lines
    → CPU sees device's data on next read, no CPU invalidate needed
  
  → Both coherent AND streaming DMA work without cache ops!
    dma_map_single() with coherent interconnect: just returns physical address
    No DC CIVAC/IVAC needed
  
  ARM64 SoC examples with coherent DMA:
    AWS Graviton 3 (Neoverse V1): GIC + CMN-700 + PCIe coherent
    Apple M1: custom coherent fabric (CPU + GPU + Neural Engine)
    Qualcomm Snapdragon 8 Gen 2: SMMU + coherent interconnect
    Ampere Altra (Neoverse N1): CCIX/PCIe coherent

CASE 2: Non-coherent DMA master (embedded SoCs, older designs)
  Bus: simple AXI (no coherency extension)
  Device DMA: goes directly to DRAM, bypasses CPU caches
  CPU cache: NOT snooped by device → stale data possible
  
  Problems:
    a. CPU has dirty cache lines that device doesn't see:
       CPU writes packet data → still in L1/L2 cache
       DMA transmit device reads from DRAM → sees OLD data!
       Fix: DC CIVAC (Clean and Invalidate to PoC) before device reads
    
    b. Device writes new data to DRAM, CPU cache is stale:
       DMA receive device writes packet to DRAM
       CPU reads → gets L1/L2 cache copy (old data!)
       Fix: DC IVAC (Invalidate) to evict CPU cache lines before CPU reads
       OR: allocate as non-cacheable (MAIR.Attr = Device nGnRE or Normal NC)

ARM64 cache maintenance instructions for DMA:
  DC CIVAC, x0:  Clean and Invalidate by VA to PoC
                 "PoC" = Point of Coherency = DRAM
                 Flushes CPU dirty cache → DRAM, then invalidates
                 Use: before device reads (ensure DRAM has fresh data)
  
  DC IVAC, x0:   Invalidate by VA to PoC (no writeback!)
                 Discards CPU cache copy without writing back
                 Use: before CPU reads (discard stale cache, re-read from DRAM)
                 WARNING: if cache line is dirty → data LOST! Must not use
                          on regions that CPU might have written to.
                          Only use on regions that are exclusively device-written.
  
  DC CVAC, x0:   Clean by VA to PoC (write back, keep in cache)
                 Safer than CIVAC: writes back but doesn't discard
                 Less aggressive; doesn't evict from cache
  
  DSB ISH:       Data Synchronization Barrier (Inner Shareable)
                 MUST follow DC CIVAC: ensures cache op completes before
                 proceeding. Without DSB: subsequent DMA may start before
                 cache flush is visible to memory controller.

Complete sequence for non-coherent streaming DMA:

  dma_map_single(dev, cpu_ptr, size, DMA_TO_DEVICE):
    → arch_sync_dma_for_device():
        for each cache line in [cpu_ptr, cpu_ptr+size):
          DC CIVAC, addr  // flush dirty lines to DRAM
        DSB ISH           // wait for all flushes to complete
    → returns dma_addr (IOVA or PA)
  
  [device performs DMA read from DRAM]
  
  dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE):
    → Nothing for DMA_TO_DEVICE (device reads, CPU doesn't need to re-sync)
    → Actually: arch_sync_dma_for_cpu() is a no-op for DMA_TO_DEVICE
  
  ─────────────────────────────────────────────────
  
  dma_map_single(dev, cpu_ptr, size, DMA_FROM_DEVICE):
    → arch_sync_dma_for_device():
        for each cache line in [cpu_ptr, cpu_ptr+size):
          DC CIVAC, addr  // remove any dirty CPU data (stale after device writes)
        DSB ISH
    → returns dma_addr
  
  [device performs DMA write to DRAM]
  
  dma_unmap_single(dev, dma_addr, size, DMA_FROM_DEVICE):
    → arch_sync_dma_for_cpu():
        for each cache line:
          DC IVAC, addr   // invalidate CPU cache (force re-read from DRAM)
        DSB ISH
  
  CPU now reads from cpu_ptr: gets device's written data ✓
```

---

## 3. Coherent DMA: dma_alloc_coherent()

```c
/*
 * dma_alloc_coherent: allocate a buffer that is DMA-coherent
 * Returns: CPU virtual address (coherent access guaranteed)
 *          dma_handle: DMA address for device
 */
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag);

ARM64 dma_alloc_coherent() implementation:

Path 1: IOMMU present (arm_smmu as IOMMU):
  dma_alloc_from_coherent() →
  iommu_dma_alloc_coherent():
    1. alloc_pages(GFP_KERNEL | __GFP_ZERO): allocate physical pages
       (Normal WB cacheable, if hardware coherent)
    2. alloc_iova(iovad, size): allocate IOVA in device's address space
    3. iommu_map(domain, iova, phys, size, IOMMU_READ|IOMMU_WRITE):
       create SMMU mapping: IOVA → PA
    4. arch_dma_set_uncached(cpu_va, size):
       IF hardware NOT coherent: remap VA as non-cacheable
         Uses fixmap or vmalloc with MT_NORMAL_NC (Normal Non-Cacheable MAIR attr)
         MAIR_EL1.Attr = 0b01000100 (Normal Non-Cacheable)
       IF hardware IS coherent: leave as Normal WB (fast, coherent)
    5. return: cpu_va = coherent CPU mapping
               *dma_handle = IOVA (device uses this)

Path 2: No IOMMU (coherent device, direct mapping):
  dma_direct_alloc():
    1. alloc_pages(GFP_KERNEL | GFP_DMA): ensure within device DMA range
    2. arch_dma_set_uncached() OR leave Normal WB (if coherent bus)
    3. *dma_handle = phys_to_dma(dev, page_to_phys(page))
       (may subtract dma_pfn_offset for devices with DMA mask constraint)
    4. return: __va(page_to_phys(page)) (or remapped non-cached VA)

dma_alloc_coherent properties:
  → Buffer persists until dma_free_coherent() is called
  → CPU can read/write at any time without cache maintenance
  → Device can read/write at any time without cache maintenance
  → Both see each other's changes immediately (coherent)
  → Performance: slower if non-cacheable (Normal NC = no cache benefit)
                 same as regular memory if coherent hardware

Use cases for dma_alloc_coherent():
  DMA descriptor rings (e.g., NIC TX/RX descriptor ring)
    CPU writes descriptors, device reads them, device writes status
    Both access continuously → coherent buffer is correct choice
  
  Shared control registers (mailbox between CPU and DSP firmware)
  DMA completion status buffers
  Interrupt status registers (device writes, CPU reads frequently)

Pitfall: do NOT use dma_alloc_coherent for large transfer data buffers
  Example: 1GB video frame → allocating 1GB as coherent (non-cached) is wasteful
  Better: allocate normal memory + streaming DMA map for the transfer
```

---

## 4. Streaming DMA: dma_map_sg() for Scatter-Gather

```c
/*
 * dma_map_sg: map a scatter-gather list for DMA
 * Returns: number of DMA segments (may be fewer than nents due to merging)
 */
int dma_map_sg(struct device *dev, struct scatterlist *sg,
               int nents, enum dma_data_direction dir);

Scatter-gather: device DMA from/to non-contiguous physical memory
  Physical memory: page A at PA=0x1000, page B at PA=0x5000, page C at PA=0x9000
  Without IOMMU: 3 separate DMA transfers (device must support S/G)
  With IOMMU: map A,B,C into contiguous IOVA space → device sees contiguous range!
  
  This is IOMMU's killer feature for DMA:
    Presents contiguous IOVA even for physically fragmented buffers
    Devices without scatter-gather hardware: works transparently!

dma_map_sg() with IOMMU:
  1. For each scatterlist entry: iommu_map(domain, iova_ptr, page_pa, page_size, ...)
     iova_ptr advances by page_size each iteration
  2. Try to merge consecutive IOVAs: iommu_dma_map_sg() coalesces when possible
  3. Caller: iterates sg using for_each_sg(sg, s, nents, i):
     s->dma_address = IOVA for this segment
     s->dma_length = length of this segment

dma_map_sg() without IOMMU (direct):
  1. For each scatterlist entry: s->dma_address = phys_to_dma(dev, sg_phys(s))
  2. Cache maintenance per entry (if non-coherent): DC CIVAC per page
  3. Returns nents (same count — no merging without IOMMU)

Unmap and sync:
  dma_unmap_sg(dev, sg, nents, dir):
    Removes IOMMU mappings
    Cache maintenance for DMA_FROM_DEVICE (DC IVAC)
  
  dma_sync_sg_for_cpu(dev, sg, nents, DMA_FROM_DEVICE):
    Used when reusing buffer without unmap/remap
    Cache invalidation only (CPU needs to see device data)
  
  dma_sync_sg_for_device(dev, sg, nents, DMA_TO_DEVICE):
    Used when reusing buffer without unmap/remap
    Cache flush (DC CIVAC) to make CPU writes visible to device

ARM64 cache line size: 64 bytes (Cortex-A55, A76, Neoverse N1)
  Cache ops granularity: DCZID_EL0.BS field
  DC CIVAC performance: ~50 cycles per line (approximate)
  For 4MB buffer: 65536 cache ops × 50 cycles = ~3.2M cycles → noticeable!
  → Coherent interconnect avoids all this overhead
```

---

## 5. Hardware Coherency: ARM Interconnect Deep Dive

```
ARM coherency interconnect evolution:

CCI-400 (Cache Coherent Interconnect 400):
  ARM Cortex-A53/A57 era (2013-2016)
  Supports: 2 ACE masters (CPU clusters) + multiple ACE-Lite masters
  ACE-Lite: DMA coherency (snoop only, no cache invalidation broadcast)
  Bandwidth: limited
  
CCI-550 / CCI-600:
  ARM Cortex-A72/A73 era
  Up to 6 ACE masters
  Larger snoop filter (reduces unnecessary snoops)

CCN-512:
  Server-grade, mesh topology
  ARM Neoverse E1 clusters
  CHI (Coherent Hub Interface) protocol

CMN-600 / CMN-700 (Coherent Mesh Network):
  ARM Neoverse N1/N2/V2 (server, AWS Graviton, Ampere Altra)
  Mesh interconnect: CPU clusters + PCIe + DRAM as nodes
  Protocol: AMBA CHI (replaces ACE for high-performance)
  Nodes:
    HN-F (Home Node Fully coherent): LLC (shared cache) + directory
    RN-I (Requester Node I/O): connects non-CHI devices (PCIe, SMMU)
    RN-D (Requester Node D): same but with DVM (Distributed Virtual Memory) support
    SN-F (Subordinate Node Fully coherent): DRAM controller
  
  CMN-700 coherency guarantee:
    Device DMA through RN-I: RN-I sends snoop requests to HN-F
    HN-F directory tracks which HN-F / CPU cache has the data
    If CPU has dirty cache line: snoop response with data
    Device gets fresh data (without CPU doing explicit flush)

Practical ARM64 server DMA setup:
  Neoverse N1 (AWS Graviton 2):
    CMN-600 mesh
    PCIe RC connected via RN-I node
    All NVMe/Ethernet/SMMU DMA: coherent via CMN-600
    → DMA_ATTR_NON_COHERENT not needed
    → dma_map_single() = just IOVA allocation (no cache ops)
    → Significant performance benefit over non-coherent path

Detecting coherency in Linux:
  ARM64: ACPI IORT (I/O Remapping Table):
    Node type: Root Complex, SMMU, Named Component
    DMA-coherent flag: bit in IORT node flags
    → kernel: dev->dma_coherent = true
    → arch_sync_dma_for_device() = no-op (coherent, skip cache ops)
  
  Device tree:
    dma-coherent; property in device node
    → also sets dev->dma_coherent
  
  Without dma-coherent: assume non-coherent → cache ops on every map/unmap
```

---

## 6. SWIOTLB: Software Bounce Buffers

```
SWIOTLB (Software I/O Translation Lookaside Buffer):
  Legacy mechanism for 32-bit DMA devices on 64-bit systems WITHOUT IOMMU
  
  Problem:
    Device DMA mask: 32-bit (cannot address above 4GB)
    System RAM: 64-bit (pages may be at physical address > 4GB)
    Without IOMMU: cannot map high PA to low IOVA (no hardware)
    Solution: bounce buffer in low memory (< 4GB)
  
  SWIOTLB layout:
    Kernel boot: io_tlb_pool = alloc_bootmem_low(SWIOTLB_SIZE) // typically 64MB
    io_tlb_nslabs: number of 2KB slots in SWIOTLB area
    io_tlb_orig_addr[]: track where original buffer is (for copy-back)

  dma_map_single() with SWIOTLB:
    PA = page_to_phys(virt_to_page(ptr))
    if (PA + size > dev->coherent_dma_mask):
      // Buffer is above device's DMA range — need bounce buffer
      io_tlb_slot = swiotlb_find_slots(size)
      io_tlb_orig_addr[slot] = original_ptr
      if (dir == DMA_TO_DEVICE):
        memcpy(swiotlb_slot, ptr, size)  // copy up front
      return: dma_addr = io_tlb_phys_addr + slot * slot_size  // low PA
    else:
      return: dma_addr = PA  // no bounce needed
  
  dma_unmap_single() with SWIOTLB:
    if (was_bounced):
      if (dir == DMA_FROM_DEVICE):
        memcpy(original_ptr, swiotlb_slot, size)  // copy back
      free_swiotlb_slot(slot)

  ARM64 SWIOTLB usage today:
    Mostly for: EFI/ACPI systems without IOMMU (embedded)
                Encrypted memory (AMD SME/Intel TME analogs on ARM: Realm/CCA)
                Secure EL3 (swiotlb used for DMA in EL1 that must stay below 4GB)
    With SMMU: SWIOTLB not needed (SMMU maps 32-bit IOVA → 64-bit PA)
    
  swiotlb on ARM64 Realm (ARMv9 CCA):
    Realm VMs: memory is encrypted, DMA devices cannot access encrypted RAM
    swiotlb acts as shared (unencrypted) bounce buffer for device DMA
    mem_encrypt_skip_lpage: control granularity
    → coco_mem_enc → use_swiotlb for all DMA
```

---

## 7. Interview Questions & Answers

**Q1: When must you call dma_sync_single_for_cpu() and when can you skip it?**

You must call `dma_sync_single_for_cpu()` (or `dma_unmap_single()` which calls it internally) whenever:

1. **DMA_FROM_DEVICE**: device wrote data to the buffer, CPU needs to read it. Without the sync, the CPU's cache may contain stale data. The sync call (`arch_sync_dma_for_cpu()`) executes `DC IVAC` to invalidate CPU cache lines, forcing the CPU to re-read from DRAM (where the device wrote).

2. **Non-coherent interconnect**: only required on platforms where the DMA master is NOT on a hardware-coherent bus. If `dev->dma_coherent == true` (set via `dma-coherent` DT property or ACPI IORT flag), the hardware interconnect (e.g., CMN-700) snoop-tracks all cache lines and `arch_sync_dma_for_cpu()` is a no-op.

You can skip `dma_sync_single_for_cpu()` **only if** you call `dma_unmap_single()` first. Unmapping includes the sync. But if you're keeping the mapping alive and reusing the buffer (optimization to avoid IOMMU overhead), you MUST call `dma_sync_single_for_cpu()` before CPU reads and `dma_sync_single_for_device()` before device reads.

**Q2: What is the performance difference between dma_alloc_coherent and streaming DMA?**

`dma_alloc_coherent()` on non-coherent hardware allocates memory mapped as **Normal Non-Cacheable** (MAIR = 0b01000100). Benefits: no sync calls ever needed. Downside: every CPU access is slow (goes to DRAM, no L1/L2 cache benefit). For descriptor rings (small, accessed by both CPU and device frequently) this is acceptable. For large data buffers: catastrophic — a 1MB buffer copied by CPU at Normal-NC speed: much slower than cached.

`dma_map_single()` with streaming DMA uses **Normal WB** (cached) memory. CPU accesses are fast (cached). The sync calls (`DC CIVAC`, `DC IVAC`) add overhead at map/unmap time, proportional to buffer size (one instruction per 64-byte cache line). For 4KB page: ~64 cache ops. For 1MB: ~16K cache ops ≈ a few microseconds — negligible vs. typical DMA transfer latency.

On coherent hardware (CMN-700, ACE-Lite): both paths have zero cache maintenance overhead. `dma_alloc_coherent()` may still allocate Normal-NC for compatibility (safe, but slower CPU access). Better option on coherent platforms: use `DMA_ATTR_NON_CONSISTENT` with streaming DMA to get cached memory without sync overhead.

---

## 8. Quick Reference

| Operation | Coherent DMA | Streaming DMA |
|---|---|---|
| Allocation | `dma_alloc_coherent()` | Any memory (kmalloc, get_user_pages) |
| Mapping | Not needed | `dma_map_single()` / `dma_map_sg()` |
| CPU write → device reads | No sync needed | `dma_sync_single_for_device()` or included in map |
| Device writes → CPU reads | No sync needed | `dma_sync_single_for_cpu()` or included in unmap |
| Lifetime | Persistent | Per-transaction |
| Cache type | Non-cached (NC) or WB on coherent | Cached WB |
| Best for | Descriptor rings, control structs | Large data transfers |

| ARM64 Cache Op | When Used |
|---|---|
| `DC CIVAC` | Before device reads (flush dirty lines to DRAM) |
| `DC IVAC` | Before CPU reads (discard stale, re-read from DRAM) |
| `DSB ISH` | After DC ops (ensure completion) |
| None | Coherent interconnect (hardware handles it) |
