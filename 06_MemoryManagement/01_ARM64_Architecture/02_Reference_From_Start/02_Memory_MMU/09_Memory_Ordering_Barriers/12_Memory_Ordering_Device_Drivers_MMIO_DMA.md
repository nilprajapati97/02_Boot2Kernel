# Memory Ordering in Device Drivers: MMIO and DMA

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. MMIO Memory Ordering

```
MMIO (Memory-Mapped I/O): device registers accessed via load/store instructions
  to a specific physical address range mapped as Device memory.

Device memory attributes on ARM64:
  Device nGnRnE: strictest (PCIe config space)
    nG = non-Gathering: each access is separate (no combining)
    nR = non-Reordering: accesses ordered relative to the SAME device
    nE = non-Early: write completion acknowledged only AFTER device receives it
    
  Device nGnRE: most MMIO registers
    Same as above but: Early write completion allowed (CPU needn't wait for device)
    The write leaves the CPU's store buffer "quickly" but device may not have received it
    
  Device GRE: write-combining (framebuffer)
    Gathering: small writes may be combined into larger bus transactions
    Reordering: may be reordered

The nR (non-Reordering) guarantee:
  For Device nGnRE memory: accesses to the SAME device (same memory region) are ordered
  BUT: accesses between DIFFERENT devices are NOT ordered
  AND: Device accesses relative to Normal memory accesses are NOT ordered without barriers

Barrier requirements for MMIO:
  After writing to a config register, before writing to a trigger register:
    Uses same device: Device nGnRE nR ordering handles this!
    → No barrier needed between config writes to SAME device
    
  Writing Normal memory data, then writing MMIO DMA start:
    Different regions: Normal memory + Device memory = NO ordering!
    Need: wmb() = DSB ST before the MMIO write
    
  Reading MMIO status, then reading Normal memory:
    Device read may complete, but Normal memory read may be issued earlier
    Need: rmb() = DSB LD between MMIO status read and Normal memory read
```

---

## 2. Linux MMIO Helper Functions

```
Linux provides MMIO accessor functions (include/asm/io.h):

Read functions:
  readb(addr)  → 8-bit  MMIO read  (DSB before return)
  readw(addr)  → 16-bit MMIO read  (DSB before return)
  readl(addr)  → 32-bit MMIO read  (DSB before return)
  readq(addr)  → 64-bit MMIO read  (DSB before return)
  
Write functions:
  writeb(val, addr) → 8-bit  MMIO write (DSB before write)
  writew(val, addr) → 16-bit MMIO write (DSB before write)
  writel(val, addr) → 32-bit MMIO write (DSB before write)
  writeq(val, addr) → 64-bit MMIO write (DSB before write)

ARM64 implementation (arch/arm64/include/asm/io.h):
  #define readl(addr) \
      ({ u32 __v = __raw_readl(addr); __iormb(__v); __v; })
  
  #define writel(val, addr) \
      ({ __iowmb(); __raw_writel((val), addr); })
  
  __iormb(): IO read memory barrier = DSB LD (wait for loads to complete)
  __iowmb(): IO write memory barrier = DSB ST (wait for stores before write)
  
  __raw_readl/__raw_writel: raw access without barriers
  (Use __raw_ variants ONLY when you know the ordering is handled externally)

Platform ordering example:
  // DMA configuration:
  writel(dma_src_addr, DMA_SRC_REG);   // writel includes DSB before
  writel(dma_dst_addr, DMA_DST_REG);   // ordered (same device, nR)
  writel(dma_length,   DMA_LEN_REG);   // ordered
  wmb();                               // DSB ST: ensure all writes before START
  writel(DMA_GO_BIT,   DMA_CTL_REG);  // write START register
  // DMA controller starts operating
```

---

## 3. DMA Coherency and Barriers

```
DMA coherency problem:
  CPU writes data to DRAM via write-back cache
  DMA engine reads from DRAM: reads STALE data (before CPU writeback)!
  
  OR:
  DMA engine writes results to DRAM
  CPU reads from L1 cache: reads OLD data (before DMA write)!

Linux DMA API (include/linux/dma-mapping.h):

dma_map_single(dev, ptr, size, DMA_TO_DEVICE):
  ARM64 implementation:
    DC CVAC for each cache line  // clean dirty CPU data to DRAM (PoC)
    DSB ISH                      // wait for clean to COMPLETE
    Returns: DMA address (IOVirtual → Physical via SMMU if present)
  Effect: CPU's dirty cache lines written to DRAM → DMA can read from DRAM

dma_map_single(dev, ptr, size, DMA_FROM_DEVICE):
  ARM64 implementation:
    DC IVAC for each cache line  // invalidate CPU's cache copy
    DSB ISH                      // wait for invalidation
  Effect: CPU's cache copy discarded → CPU reads fresh data from DRAM
  DANGER: Only safe if CPU has NOT dirtied those cache lines after DMA started!
  
dma_map_single(dev, ptr, size, DMA_BIDIRECTIONAL):
  ARM64 implementation:
    DC CIVAC for each cache line // clean+invalidate (safe for both directions)
    DSB ISH                      
  
dma_unmap_single(dev, dma_addr, size, DMA_FROM_DEVICE):
  MANDATORY after DMA completes: unmap and re-enable CPU access
  ARM64: DC IVAC for each cache line + DSB ISH
  After unmap: CPU can safely read DMA results (stale cache cleared)
  
  Without dma_unmap_single: CPU may read stale cached data even after DMA completion!

Coherent DMA allocations:
  dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL):
    Allocates UNCACHED memory (Non-Cacheable Normal memory on ARM64)
    No cache maintenance needed: CPU and DMA always see same DRAM data
    
    ARM64: MAIR_EL1 Normal NC encoding
    PTE: Normal NC memory type (0x44 in MAIR)
    
    Trade-off: slower for CPU (cache miss on every access) but no flush needed
    Use for: control rings, descriptor queues (written by CPU, read by DMA and vice versa)
```

---

## 4. SMMU and Memory Ordering

```
SMMU (System MMU): IOMMU for ARM64 systems
  Translates Device Virtual Addresses (IOVA) → Physical Addresses (PA)
  Provides: device isolation, memory protection, remapping
  
  Impact on DMA ordering:
    With SMMU: DMA address != Physical address
    dma_map_single(): maps the physical buffer into IOVA space via SMMU
    dma_unmap_single(): unmaps from IOVA space + SMMU TLB invalidation
    
    SMMU TLB invalidation:
      SMMU has its own TLB (different from CPU TLB)
      CMD_TLBI_NHALL: invalidate all SMMU TLBs
      After iommu_unmap(): SMMU processes the invalidation command
      SMMU CMD SYNC: wait for SMMU TLB invalidation to complete
      
    Ordering concern: CPU updates page tables for IOMMU
      Must flush CPU cache of page table entries (DC CIVAC)
      DSB ISH (ensure CPU writes reach DRAM, visible to SMMU)
      Write SMMU TLB invalidation command
      SMMU CMD SYNC (wait for SMMU ack)
      → Now SMMU uses new mappings

PCIe DMA ordering:
  PCIe: separate memory ordering domain from CPU
  Posted writes: PCIe stores may complete at CPU before reaching device
  
  Ordering primitives:
    CPU → PCIe device: wmb() + writel() (register write = fence)
    PCIe device → CPU: rmb() + readl() (register read = completion fence)
    
  PCIe spec: a PCI read (readl) implicitly flushes all prior PCI writes
  Linux: use readl() after a sequence of writel() to confirm write completion
    writel(val, addr);
    (void)readl(addr);  // read-back: ensures write reached device
```

---

## 5. ioremap and Memory Attributes

```
ioremap() and memory type selection:

ioremap(phys_addr, size):
  Default: Device nGnRE (PROT_DEVICE_nGnRE)
  For most MMIO registers: correct choice

ioremap_nocache(phys_addr, size):
  Deprecated, same as ioremap() on ARM64
  (All MMIO is "non-cacheable" in Device memory)

ioremap_wc(phys_addr, size):
  Write-combining: for framebuffers, large MMIO BARs
  ARM64: Device nGRE or Normal NC (platform-dependent)
  Allows write combining → better performance for bulk writes

ioremap_wt(phys_addr, size):
  Write-through: for certain device memory
  Some platforms use Normal WT memory type

Accessing ioremap'd memory:
  NEVER use direct pointer dereferences:
    u32 *reg = ioremap(DEVICE_BASE, 0x1000);
    *reg = value;    // WRONG: no barrier, compiler may optimize

  ALWAYS use readl/writel:
    void __iomem *base = ioremap(DEVICE_BASE, 0x1000);
    writel(value, base + REG_OFFSET);    // CORRECT: includes barrier
    u32 status = readl(base + STATUS_OFFSET);  // CORRECT
    iounmap(base);   // unmap when done

  The __iomem annotation: GCC sparse checker
    __iomem: marks pointer as I/O mapped memory
    Accessing without readl/writel: sparse generates a warning
    Enforces correct usage of MMIO accessors in kernel code
```

---

## 6. Interview Questions & Answers

**Q1: A DMA operation completes (interrupt received), but the CPU reads stale data from the DMA destination buffer. What happened and how do you fix it?**

This is a DMA cache coherency bug. When the DMA controller writes data to DRAM, it bypasses the CPU's L1/L2/L3 cache hierarchy (DMA is not cache-coherent with the CPU on non-coherent platforms). If the CPU had previously read from that buffer (populating its cache with old data) and had not invalidated those cache lines before/after the DMA, the CPU continues reading from its local cache (stale data) instead of DRAM (new DMA data).

**Root cause**: the CPU's cache was not invalidated after the DMA write completed.

**Fix**: 
1. Before starting a `DMA_FROM_DEVICE` transfer: call `dma_map_single(dev, buf, size, DMA_FROM_DEVICE)` which executes `DC IVAC` for all cache lines in the buffer + `DSB ISH`. This invalidates the CPU's stale copy before DMA starts writing.
2. After DMA completes (in the interrupt handler or polling loop): call `dma_unmap_single(dev, dma_addr, size, DMA_FROM_DEVICE)` which re-invalidates any CPU cache lines that might have been re-populated between the map and DMA completion.
3. After `dma_unmap_single`: CPU reads are guaranteed to see the fresh DMA data from DRAM.

On **coherent** systems (ARM64 with CCI/CCN-based interconnect where devices join the coherency domain): DMA is cache-coherent and no `DC IVAC` is needed. Use `dma_alloc_coherent()` for the most portable solution.

---

## 7. Quick Reference

| Scenario | Barrier | Linux Function |
|---|---|---|
| CPU data → DMA read | DC CVAC + DSB ISH | dma_map_single(TO_DEVICE) |
| DMA write → CPU read | DC IVAC + DSB ISH | dma_map_single(FROM_DEVICE) |
| Bidirectional | DC CIVAC + DSB ISH | dma_map_single(BIDIRECTIONAL) |
| MMIO write sequence | DSB ST before writel | writel() built-in |
| MMIO read result | DSB LD after readl | readl() built-in |
| SMMU page table update | DC CIVAC + DSB + SMMU CMD SYNC | iommu_map() |

| DMA Type | CPU Access | Barrier Needed |
|---|---|---|
| Streaming (map_single) | Cached | DC CIVAC/CVAC/IVAC + DSB |
| Coherent (alloc_coherent) | Non-cached | None |
| Bounce buffer (SWIOTLB) | Cached (internal copy) | Handled by SWIOTLB |
