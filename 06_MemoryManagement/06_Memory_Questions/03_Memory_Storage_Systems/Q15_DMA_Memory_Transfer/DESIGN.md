# Q15 — Design a DMA-Based Memory Transfer System

---

## 1. Problem Statement

DMA (Direct Memory Access) allows devices to transfer data to/from system memory without CPU involvement. A well-designed DMA subsystem must:
- Provide CPU-independent bulk transfers at full PCIe bandwidth.
- Handle IOMMU address translation for device DMA safety.
- Support scatter-gather DMA for non-contiguous physical memory.
- Integrate coherency: CPU and device must see consistent data.
- Handle RDMA and GPU peer-to-peer DMA (P2P across PCIe).

Design the complete DMA transfer system including engine management, descriptor rings, coherency model, and error handling.

---

## 2. Requirements

### 2.1 Functional Requirements
- DMA engine (IOAT / NVIDIA copy engine) for host↔device transfers.
- Scatter-gather list (SGL) support for fragmented physical memory.
- IOMMU mapping: translate host virtual/physical addresses to device-visible DMA addresses.
- Coherent DMA (CPU-visible without explicit cache flush) and streaming DMA (explicit sync).
- DMA completion notification: interrupt or polling.
- Error handling: IOMMU fault, ECC error, DMA timeout.

### 2.2 Non-Functional Requirements
- Throughput: ≥ 95% of PCIe bandwidth (e.g., 32 GB/s for PCIe 4.0 x16).
- Latency for 4KB DMA: < 5 µs.
- Zero CPU cycles during data movement.
- Safe: IOMMU prevents device from accessing unauthorized memory.

---

## 3. Constraints & Assumptions

- x86-64 with Intel VT-d IOMMU.
- PCIe device (GPU or NIC) with DMA engine.
- Linux DMA API (`dma_map_*`, `dma_alloc_coherent`).
- IOMMU enabled in passthrough or translate mode.
- Cache coherency: x86 PCIe devices are coherent (MESI + PCIe snooping).

---

## 4. Architecture Overview

```
  CPU (x86-64)                       Device (GPU/NIC)
  ┌─────────────────────────┐        ┌─────────────────────────┐
  │  Kernel Driver          │        │  DMA Engine             │
  │  ┌───────────────────┐  │        │  ┌──────────────────┐   │
  │  │ DMA Descriptor    │  │  PCIe  │  │ Descriptor Ring  │   │
  │  │ Ring (MMIO write) │──┼────────┼─►│ Fetch → Execute  │   │
  │  └───────────────────┘  │        │  └──────────────────┘   │
  │                         │        │           │              │
  │  ┌───────────────────┐  │        │           ▼ DMA Read    │
  │  │ IOMMU Mapping     │  │        │  ┌──────────────────┐   │
  │  │ VA/PA → IOVA      │  │        │  │  PCIe TLP Read   │◄──┤
  │  └───────────────────┘  │        │  │  Request         │   │
  │                         │        │  └──────────────────┘   │
  │  System RAM (DRAM)      │        │                         │
  │  ┌───────────────────┐  │        │  DMA Write              │
  │  │  Source Buffer    │◄─┼────────┼──(TLP Write)            │
  │  │  Destination Buf  │──┼────────┼──►DMA Read target       │
  │  └───────────────────┘  │        └─────────────────────────┘
  └─────────────────────────┘
         ▲
         │  IOMMU translates device DMA addresses → physical addresses
  ┌──────┴──────────┐
  │  Intel VT-d     │
  │  IOMMU          │
  │  DMAR tables    │
  └─────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Linux DMA Mapping Abstraction

```c
/* Coherent DMA allocation (CPU + device see same memory, always) */
void *cpu_addr = dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL);
/*
 * cpu_addr: kernel virtual address
 * dma_handle: bus address for device (IOVA if IOMMU present, PA if not)
 * Memory is IOMMU-mapped + CPU-mapped; no cache management needed
 */

/* Streaming DMA (explicit sync, one-directional) */
dma_addr_t dma_handle = dma_map_single(dev, cpu_addr, size, DMA_TO_DEVICE);
/*
 * Maps existing CPU buffer to device DMA address.
 * CPU must call dma_sync_single_for_device() before device DMA.
 * CPU must call dma_sync_single_for_cpu() before CPU access after device DMA.
 */

/* Scatter-gather DMA */
int nents = dma_map_sg(dev, sg_list, sg_count, DMA_FROM_DEVICE);
/* Maps each scatterlist entry to a DMA address */
/* IOMMU can coalesce adjacent pages into contiguous IOVA ranges */
```

### 5.2 DMA Descriptor (device-specific, example: NVIDIA copy engine)

```c
/* NVIDIA DMA descriptor format (simplified) */
struct nv_dma_descriptor {
    u64  src_addr;      /* source DMA address (IOVA or GPU VA) */
    u64  dst_addr;      /* destination DMA address */
    u32  size;          /* transfer size in bytes */
    u32  flags;         /* NV_DMA_COPY_FLAGS_COMPLETION_NOTIFY,
                           NV_DMA_COPY_FLAGS_FENCE_ENABLE */
    u64  completion_addr; /* address to write completion status */
    u32  semaphore_val; /* value to write at completion */
    u32  padding;
} __attribute__((aligned(64)));
```

### 5.3 DMA Ring Buffer (submission queue)

```c
struct dma_ring {
    struct dma_descriptor  *desc;       /* descriptor array (coherent DMA mem) */
    dma_addr_t              desc_pa;    /* device-visible address of desc[] */
    u32                     capacity;   /* power-of-two */
    u32                     mask;       /* capacity - 1 */
    u32                     head;       /* next descriptor to submit */
    u32                     tail;       /* next descriptor to complete */
    void __iomem           *doorbell;   /* MMIO register: write tail to kick engine */
    spinlock_t              lock;       /* protects head/tail */
    /* Completion tracking */
    struct completion       completions[]; /* per-descriptor completion events */
};
```

### 5.4 IOMMU Domain

```c
struct iommu_domain {
    const struct iommu_ops *ops;
    void                   *iova_cookie;   /* IOVA allocator state */
    unsigned int            type;          /* IOMMU_DOMAIN_DMA, IOMMU_DOMAIN_IDENTITY */
    struct iommu_dma_cookie *cookie;
};

/* IOVA space: virtual address space visible to this device */
struct iommu_dma_cookie {
    struct iova_domain  iovad;     /* IOVA allocator (rb-tree of free ranges) */
    struct list_head    msi_page_list;
    struct iova         *lefthalf_iovad_cache; /* per-CPU cache */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 IOMMU Mapping Pipeline

```
Host Physical Address (PA) → IOMMU → IOVA (device-visible DMA address)

Steps:
1. Allocate IOVA range: iova_alloc(&iovad, size, &iova)
   → rb-tree search for free IOVA range of required size
   → return iova.pfn_lo, iova.pfn_hi (IOVA start/end page frames)

2. Write IOMMU page table entries:
   iommu_map(domain, iova, pa, size, IOMMU_READ | IOMMU_WRITE)
   → For each page in range:
       domain->ops->map(domain, iova + i*PAGE_SIZE, pa + i*PAGE_SIZE, PAGE_SIZE)
   → Intel VT-d: updates context table → PASID table → second-level page table
   → Flush IOMMU TLB: qi_flush_tlb(iommu, iova, size)

3. Device programs DMA with IOVA address
4. IOMMU translates IOVA → PA for each DMA TLP
5. After DMA: iommu_unmap() + iova_free()
```

### 6.2 Coherent vs Streaming DMA — Cache Coherency

**On x86 with PCIe:** All DMA is coherent — PCIe snoop filter participates in MESI protocol.
- Device write: PCIe write TLP → MESI bus → CPU sees updated data without explicit flush.
- CPU write before DMA read: store buffer flushed by PCIe write ordering.

**On ARM / weakly-ordered systems:** DMA is NOT automatically coherent.
```c
/* Before device DMA read: flush CPU caches */
dma_sync_single_for_device(dev, dma_handle, size, DMA_TO_DEVICE);
    → arch_clean_and_invalidate_cache_range(cpu_addr, size)

/* After device DMA write: invalidate CPU caches */
dma_sync_single_for_cpu(dev, dma_handle, size, DMA_FROM_DEVICE);
    → arch_invalidate_cache_range(cpu_addr, size)
```

### 6.3 Scatter-Gather DMA — Physical Fragmentation Handling

User/kernel buffers are rarely physically contiguous. SGL allows one DMA command to transfer across physically non-contiguous memory:

```c
struct scatterlist sg[MAX_SEGMENTS];
sg_init_table(sg, MAX_SEGMENTS);

/* Build SGL from vmalloc buffer (physically non-contiguous) */
vaddr = vmalloc_buffer;
for (i = 0; i < npages; i++) {
    struct page *page = vmalloc_to_page(vaddr + i * PAGE_SIZE);
    sg_set_page(&sg[i], page, PAGE_SIZE, 0);
}

/* IOMMU: coalesce adjacent physical pages into contiguous IOVA */
nents = dma_map_sg(dev, sg, npages, DMA_TO_DEVICE);
/* nents may be < npages if IOMMU coalesced adjacent pages */

/* Program device DMA with SGL entries */
for_each_sg(sg, entry, nents, i) {
    dma_desc[i].addr = sg_dma_address(entry);
    dma_desc[i].len  = sg_dma_len(entry);
}
```

**IOMMU coalescing benefit:** If two physically adjacent pages are adjacent in physical memory, IOMMU maps them as one IOVA region → device sees contiguous DMA address → device DMA engine uses one segment instead of two.

### 6.4 DMA Completion Notification — Interrupt vs Poll

**Interrupt mode (low-power, higher latency):**
```c
/* Device writes completion status to coherent DMA memory */
/* Then asserts MSI-X interrupt */
irqreturn_t dma_completion_handler(int irq, void *dev)
{
    u32 status = le32_to_cpu(*ring->completion_addr);
    if (status & DMA_COMPLETE) {
        ring->tail = (ring->tail + 1) & ring->mask;
        complete(&ring->completions[ring->tail]);
        return IRQ_HANDLED;
    }
    return IRQ_NONE;
}
```

**Poll mode (low-latency, high CPU cost):**
```c
/* Dedicated CPU polls completion memory */
while (!(le32_to_cpu(*ring->completion_addr) & DMA_COMPLETE))
    cpu_relax();  /* tight poll — < 5 µs */
```

**Hybrid (GPU DMA typical):** Poll for small DMA (< 64KB), interrupt for large.

### 6.5 GPU P2P DMA (Peer-to-Peer)

For GPU-to-GPU or GPU-to-NIC transfers:
```
GPU A VRAM → PCIe switch → GPU B VRAM (or NIC)
             (no system RAM involved)

Requirements:
1. Both devices on same PCIe root complex (or switch supporting P2P).
2. BAR mapping: GPU A maps GPU B's BAR2 (VRAM aperture) into its DMA address space.
3. IOMMU P2P: configure IOMMU to permit GPU A to access GPU B's IOVA range.
4. Transfer: GPU A DMA engine reads from its own VRAM, writes to GPU B's IOVA.

Linux API:
    pci_p2pdma_distance(pdev_A, pdev_B, false)  /* check P2P capability */
    pci_p2pmem_alloc_sgl(pdev, sgl, count)       /* alloc SGL from P2P region */
    dma_map_sg_attrs(dev_A, sgl, count, dir, DMA_ATTR_NO_WARN)
```

---

## 7. Trade-off Analysis

| Decision | Coherent DMA | Streaming DMA | Reason |
|---|---|---|---|
| Cache management | None required | Explicit sync | Coherent: easier; streaming: avoids bounce buffer allocation |
| Memory allocation | Dedicated (dma_alloc_coherent) | Any (dma_map_single) | Coherent: allocated once; streaming: use existing buffers |
| Performance | Slightly lower (always snooped) | Slightly higher | Streaming avoids snoop overhead on read-only device writes |
| Use case | Descriptor rings, doorbell pages | Bulk data transfer | Small, frequently accessed → coherent; large bulk → streaming |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| DMA API | `kernel/dma/mapping.c` | `dma_map_single()`, `dma_alloc_coherent()` |
| Scatter-gather | `include/linux/scatterlist.h` | `sg_init_table()`, `sg_set_page()`, `for_each_sg()` |
| IOMMU core | `drivers/iommu/iommu.c` | `iommu_map()`, `iommu_domain_alloc()` |
| Intel VT-d | `drivers/iommu/intel/iommu.c` | `intel_map_page()`, `qi_flush_tlb()` |
| IOVA allocator | `drivers/iommu/iova.c` | `alloc_iova()`, `free_iova()` |
| DMA pool | `kernel/dma/pool.c` | `dma_pool_alloc()`, `dma_pool_create()` |
| PCIe P2P DMA | `drivers/pci/p2pdma.c` | `pci_p2pdma_distance()`, `pci_p2pmem_alloc_sgl()` |
| NVMe DMA | `drivers/nvme/host/pci.c` | `nvme_map_data()`, `nvme_unmap_data()` |
| RDMA DMA | `drivers/infiniband/core/device.c` | `ib_dma_map_sg()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 IOMMU Fault (Unauthorized DMA Access)
```bash
dmesg | grep "DMAR\|AMD-Vi\|iommu"
# "DMAR:[DMA Read] Request device [00:02.0] fault addr 0x7f00 [fault reason 06]"
# Fault reason 06 = page not present (device trying to access unmapped address)
# Cause: use-after-dma_unmap, or buffer freed before DMA complete
```

### 9.2 DMA Timeout
```bash
# Device DMA never completes
# Check: is doorbell register writable? (MMIO BAR mapping correct?)
# Check: IOMMU mapping covers full transfer range?
# Debug: write to BAR and read back via lspci -vvv
lspci -d "10de:" -vvv | grep "Memory at"  # NVIDIA BAR addresses
```

### 9.3 Data Corruption After DMA
```bash
# Cause: CPU accessed buffer before dma_sync_single_for_cpu() on non-coherent arch
# Debug: compare source and destination checksums
# Fix: always call dma_sync_* before CPU access of streaming DMA buffers
CONFIG_DMA_API_DEBUG=y  # catches missing sync calls in debug builds
```

### 9.4 Performance Below Expected Bandwidth
```bash
# Expected: 32 GB/s PCIe 4.0 x16
# Actual: << 32 GB/s
# Check: transfer size (< 64KB: PCIe overhead dominates → pipeline multiple DMAs)
# Check: NUMA locality (DMA to non-local node adds ~50% latency)
# Check: P2P enabled: lspci -vvv | grep "Access Control Services"
```

---

## 10. Performance Considerations

- **Descriptor ring depth:** A shallow ring (depth 8) limits outstanding DMA transfers. For NVMe @ 1M IOPS, a depth of 1024 allows 1024 in-flight 4KB DMAs simultaneously.
- **IOVA allocator per-CPU cache:** `iova_rcache` pre-allocates IOVA ranges per-CPU, avoiding rb-tree contention in `alloc_iova()` for high-frequency mapping/unmapping.
- **Huge page DMA:** 2MB huge pages reduce IOMMU page table entries by 512× and reduce IOMMU TLB pressure proportionally.
- **DMA engine parallelism:** Modern GPUs have 6+ copy engines. Submit to different engines in parallel; each has its own descriptor ring.
- **Bounce buffers:** If device DMA address space < 32-bit (legacy ISA DMA), kernel allocates "bounce buffer" in low memory, copies data — avoid by ensuring all DMA is 64-bit capable.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. IOMMU role: translates IOVA → PA, enforces access control, prevents device from accessing arbitrary physical memory.
2. Coherent vs streaming DMA tradeoff: coherent always snooped (consistent but slower), streaming has explicit sync (faster for bulk, requires care).
3. Scatter-gather: IOMMU coalesces adjacent physical pages into contiguous IOVA → device sees contiguous DMA address.
4. PCIe P2P: GPU-to-NIC without system RAM — `pci_p2pdma` API.
5. DMA descriptor rings: producer-consumer ring with doorbell register — identical pattern to GPU command queues.
6. Completion: interrupt for large transfers (save CPU), polling for small (< 5 µs latency).
7. `CONFIG_DMA_API_DEBUG` for catching missing cache sync in debug builds.
8. IOVA per-CPU cache: eliminates global lock in high-frequency DMA mapping paths.
