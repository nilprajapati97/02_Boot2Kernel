# Module 14 — Camera Memory Management (DMA, IOMMU/SMMU, dma-buf, CMA, heaps)

> **Scope:** How camera frame memory is allocated, DMA-mapped, made coherent, and shared
> zero-copy across IPs. Covers the DMA API and address types, the IOMMU/SMMU and why
> camera needs it, cache coherency, contiguous memory (CMA, reserved-memory), dma-buf
> sharing and the dma-heap allocator (the ION replacement), and the SMMU fault debugging
> that dominates camera bring-up. This connects vb2 (Module 13) to the SoC SMMU/DDR and
> ties back to your ARM64 memory-management background.

> **Cross-reference:** this builds directly on `06_MemoryManagement/02_Memory_Topics/`
> (DMA, IOMMU, cache coherency, CMA). Here we apply those to the camera datapath.

---

## 1. The camera memory problem

A camera frame is large (4K NV12 ≈ 12 MB) and must be:
- **Allocated** somewhere DMA-able (physically contiguous or SMMU-mapped).
- **Mapped** for the ISP write-master DMA (device address) *and* for the CPU (virtual).
- **Coherent** — CPU sees what the device wrote, no stale cache.
- **Shared** zero-copy with the encoder, GPU, and display (no 12 MB memcpy at 60 fps).

```
 ISP write-master ─DMA─► ??? ─► DDR ─► encoder reads ─► display shows
        │                          │
   device address            shared zero-copy?
   (IOVA via SMMU)           (dma-buf)
```

Getting this wrong = SMMU faults, stale frames, or 700 MB/s of needless memcpy.

---

## 2. Three address types (recap, applied to camera)

```c
void       *vaddr;       /* CPU kernel virtual address (CPU reads the frame) */
phys_addr_t paddr;       /* CPU physical address */
dma_addr_t  dma_addr;    /* device-visible DMA address (what the ISP DMA uses) */
```

```
 CPU side                          Device (ISP DMA) side
 vaddr ──(MMU/page tables)──► paddr ◄──(SMMU/IOMMU)── dma_addr (IOVA)

 Without SMMU: dma_addr == paddr (device sees physical addresses directly)
 With SMMU:    dma_addr = IOVA, SMMU translates IOVA → paddr
```

On ARM64 SoCs (Qualcomm, NVIDIA, NXP, TI) camera DMA almost always goes **through an
SMMU**, so `dma_addr` is an IOVA, not a physical address.

---

## 3. The DMA API in the camera context

vb2 backends (Module 13) call the DMA API for you, but you must understand it:

```c
/* Coherent (consistent) — CPU & device see the same memory, no manual sync */
void *cpu = dma_alloc_coherent(dev, size, &dma_handle, GFP_KERNEL);

/* Streaming — map existing memory for one DMA direction, sync around access */
dma_addr_t d = dma_map_single(dev, ptr, size, DMA_FROM_DEVICE);  /* capture */
...
dma_unmap_single(dev, d, size, DMA_FROM_DEVICE);  /* implies cache invalidate */

/* Set the DMA mask: how many address bits the camera DMA can drive */
dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));  /* e.g. 40-bit camera DMA */
```

- **Capture** = device writes, CPU reads → direction `DMA_FROM_DEVICE` → cache
  **invalidate** after DMA so the CPU sees fresh pixels.
- The **DMA mask** matters: if the camera DMA is 32-bit but the buffer is above 4 GB
  (without SMMU), the transfer fails or needs bounce buffers. SMMU relaxes this by
  mapping high physical pages to low IOVAs.

---

## 4. The IOMMU / SMMU — why camera needs it

The **System MMU** (ARM SMMU-500/v3, on Qualcomm called "apps_smmu", on Tegra the Tegra
SMMU) translates device IOVAs to physical addresses:

```
        ┌───────────┐  IOVA   ┌──────────┐  PA   ┌─────────┐
 ISP ──►│  SMMU     │────────►│ page      │──────►│  DDR    │
 DMA    │ (per-CB)  │         │ tables    │       │         │
        └───────────┘         └──────────┘       └─────────┘
```

Why it's essential for camera:
1. **Scatter-gather without contiguity:** non-contiguous physical pages (e.g. a dma-sg
   buffer or a dma-buf from the GPU) appear as one contiguous IOVA range to the ISP —
   no need for a giant contiguous allocation.
2. **Isolation/protection:** each camera block (CSID, IFE, ICP) gets its own **context
   bank** with stream IDs (the `iommus = <&apps_smmu 0x808 ...>` in DT, Module 7/11). A
   buggy DMA can only touch its mapped region; out-of-bounds → an **SMMU fault** instead
   of silent memory corruption.
3. **Wide addressing:** a 32-bit-limited DMA engine can reach >4 GB physical memory via
   IOVA mapping.

```
 Stream ID → Context Bank → its own page table → isolated address space per device
```

---

## 5. Cache coherency for camera DMA

ARM CPUs are typically **not** hardware-coherent with device DMA, so caches must be
maintained:

```
 Capture frame (device writes DDR, CPU reads):
   1. (optional) invalidate before DMA so no dirty lines overwrite device data
   2. device DMAs the frame into DDR
   3. INVALIDATE the CPU cache for the buffer → CPU reads fresh pixels (not stale)

 If step 3 is skipped → CPU sees old cached content → "garbage/stale frame" with a
 perfectly correct DMA address. Classic, subtle bug (also Module 13 §9).
```

- vb2's dma-contig/dma-sg backends do this automatically on `vb2_buffer_done` /
  mmap-time.
- **dma-coherent** memory avoids manual sync (often uncached/write-combine) but is
  slower for CPU access — used for small control/descriptor buffers, not big frames.
- Some platforms are coherent (ACP/CCI ports); then sync is a no-op. Know your SoC.

---

## 6. Contiguous memory: CMA and reserved-memory

When hardware needs **physically contiguous** memory (no SMMU, or a block that bypasses
it), you use:

```
 CMA (Contiguous Memory Allocator):
   - a reserved pool the kernel can also use for movable pages, reclaimed on demand
   - dma_alloc_coherent / dma-contig vb2 backend pull from CMA when needed
   - configured via DT reserved-memory or cma= cmdline / CONFIG_CMA_SIZE

 reserved-memory (DT):
   reserved-memory {
       camera_region: camera@90000000 {
           reg = <0x0 0x90000000 0x0 0x02000000>;  /* 32 MB carveout */
           no-map;  /* or reusable for CMA */
       };
   };
   &camss { memory-region = <&camera_region>; };
```

- **Carveout** (`no-map`): memory removed from the kernel, dedicated to the device —
  simple but wastes RAM when idle.
- **CMA** (`reusable`): shared with the page allocator until the device needs it —
  better utilization, but allocation can fail/stall under fragmentation/memory
  pressure → the `-ENOMEM on REQBUFS` bug.

With an SMMU you usually **don't** need contiguity (dma-sg works), which is why modern
ARM64 camera designs lean on the SMMU and avoid large CMA pools.

---

## 7. dma-buf: zero-copy sharing across IPs

`dma-buf` is the kernel framework for sharing a buffer between drivers/processes without
copying:

```
 ┌──────────┐  export   ┌────────────┐  import   ┌───────────┐
 │  camera  │──────────►│  dma-buf   │──────────►│  encoder  │
 │ (vb2 WM) │  fd        │ (1 buffer) │  fd        │  (NVENC)  │
 └──────────┘           └─────┬──────┘           └───────────┘
                              │ import
                        ┌─────▼──────┐
                        │  GPU/display│
                        └─────────────┘
```

Mechanics:
- An **exporter** (allocator) creates a `dma_buf` with ops (`attach`, `map_dma_buf`,
  `mmap`, fences).
- An **importer** calls `dma_buf_attach()` + `dma_buf_map_attachment()` to get an
  `sg_table` mapped into *its* device's SMMU context.
- Each device maps the same physical pages into its own IOVA space → one buffer, many
  devices, **zero copy**.
- **Fences** (`dma_fence`) synchronize: the encoder waits on the camera's fence before
  reading, so it never reads a half-written frame.

In V4L2 this is **DMABUF** memory mode (Module 6/13): `VIDIOC_EXPBUF` exports a vb2
buffer as a dma-buf fd; the encoder imports it.

---

## 8. dma-heap: the modern allocator (ION replacement)

**ION** was Android's buffer allocator; it's deprecated and replaced by **dma-heap**
(`/dev/dma_heap/*`), now in mainline:

```
 /dev/dma_heap/system          : normal pageable system memory heap
 /dev/dma_heap/system-uncached  : uncached variant
 /dev/dma_heap/cma             : CMA-backed contiguous heap (named per region)
 (vendor heaps: secure, carveout, etc.)
```

```c
/* userspace allocates a dma-buf from a heap, shares it to camera + encoder */
int heap = open("/dev/dma_heap/system", O_RDONLY);
struct dma_heap_allocation_data alloc = { .len = size, .fd_flags = O_RDWR };
ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc);   /* alloc.fd is the dma-buf fd */
/* QBUF that fd into V4L2 (DMABUF mode) AND into the encoder → zero-copy */
```

Android's camera HAL (Module 17) uses dma-heaps to allocate frame buffers that flow
camera → HAL → encoder/GPU/display with zero copies. **Secure heaps** back DRM/secure
camera (content protection) where the CPU can't even map the buffer.

```
 ION (legacy)  ──►  dma-heap (mainline):
   ion_alloc()        DMA_HEAP_IOCTL_ALLOC
   ion heaps          /dev/dma_heap/* heaps
   custom ioctls      standard dma-buf fd everywhere
```

---

## 9. The full memory datapath (Qualcomm example)

```
 1. dma-heap (or vb2 dma-contig/sg) allocates frame buffer → physical pages
 2. buffer exported as dma-buf fd
 3. V4L2 DMABUF QBUF → vb2 imports → maps into camera SMMU context bank
    (stream ID from DT iommus) → IOVA for the IFE BUS write-master
 4. IFE DMAs YUV frame to that IOVA (SMMU translates → physical pages in DDR)
 5. frame-done IRQ → vb2_buffer_done → cache invalidate (if cached) → DQBUF
 6. same dma-buf fd handed to the video encoder, which maps it into ITS SMMU
    context → reads the frame zero-copy, waiting on the camera's dma_fence
```

No pixel data is ever copied by the CPU — only fds and IOVA mappings move around.

---

## 10. Debugging camera memory / SMMU

```bash
# SMMU faults (the #1 camera memory bug) appear in dmesg:
dmesg | grep -iE "smmu|iommu|context fault|global fault|cb fault|FSR|FAR"
#   "arm-smmu ... Unhandled context fault: fsr=0x402, iova=0x..., fsynr=..., cb=7"
#   → iova = the bad device address, cb = context bank, decode the stream

# Which heaps exist / how much CMA is free
ls /dev/dma_heap/
cat /proc/meminfo | grep -iE "CmaTotal|CmaFree"
cat /sys/kernel/debug/cma/*/count 2>/dev/null

# dma-buf accounting
cat /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null    # who holds which buffers

# IOMMU mappings (where exposed)
ls /sys/kernel/debug/iommu/ 2>/dev/null
```

Symptom → cause:
```
 SMMU context fault, iova=X      → ISP DMA'd to an unmapped/wrong IOVA:
                                    wrong stream IDs in DT, buffer not mapped,
                                    write-master programmed with a stale/over-run addr
 -ENOMEM on REQBUFS (contig)     → CMA pool too small / fragmented (need dma-sg+SMMU
                                    or a bigger reserved-memory region)
 Stale/garbage frame, addr OK    → cache not invalidated (coherency) — Module 13 §9
 Encoder reads torn frame        → missing dma_fence sync between camera & encoder
 High memory bandwidth / drops   → no zero-copy (CPU memcpy in the path) — Module 16
 Secure camera black frame       → secure heap buffer not mappable by CPU (expected)
```

The **SMMU context fault** is the signature camera memory bug: the fault address (IOVA)
and context bank tell you exactly which device DMA went out of bounds. Decode the stream
ID against the DT `iommus` to find the culprit block.

---

## 11. Interview Q&A

**Q1. Why does a camera subsystem almost always sit behind an SMMU on ARM64?**
For scatter-gather (non-contiguous pages look contiguous to the ISP, avoiding huge
contiguous allocations), isolation/protection (each block gets a context bank; bad DMA
faults instead of corrupting memory), and wide addressing (32-bit DMA can reach >4 GB via
IOVA). It also enables clean dma-buf sharing by mapping shared pages per-device.

**Q2. You see "arm-smmu Unhandled context fault, iova=0x..., cb=7". How do you debug it?**
The ISP DMA accessed an unmapped IOVA. Use the context-bank number and the DT `iommus`
stream IDs to identify which block faulted. Check that the buffer was mapped into that
context (dma-buf attach/map), that the write-master was programmed with a valid mapped
IOVA (not a stale/over-run address), and that stream IDs in DT match the hardware.

**Q3. What is dma-buf and why is it critical for camera?**
A kernel framework for sharing one buffer's physical pages across multiple
drivers/devices via an fd, with fences for synchronization. It lets a camera frame be
written by the ISP and read by the encoder/GPU/display with zero copies — essential since
copying 12 MB frames at 60 fps would waste enormous bandwidth.

**Q4. dma-heap vs ION?**
dma-heap is the mainline replacement for Android's deprecated ION. Both allocate
dma-buf-backed buffers, but dma-heap uses a standard `/dev/dma_heap/*` interface and
standard dma-buf fds (system, cma, secure heaps), integrating cleanly with the upstream
dma-buf framework instead of ION's custom ioctls/heaps.

**Q5. When do you need CMA for camera, and what's its downside?**
When hardware requires physically contiguous memory (no SMMU, or a block bypassing it).
CMA provides a reclaimable contiguous pool. Downside: under memory pressure/fragmentation
the contiguous allocation can fail or stall (`-ENOMEM`/latency). With an SMMU you can use
dma-sg and avoid CMA entirely.

**Q6. Explain the cache-coherency step for a captured frame and the bug if it's missed.**
After the device DMAs a frame into DDR, the CPU cache for that buffer must be invalidated
so the CPU reads the freshly written pixels, not stale cached lines. If skipped, you get a
garbage/stale image *despite a correct DMA address* — vb2 normally handles this, but
performance hacks that bypass sync reintroduce it.

**Q7. How does a frame travel zero-copy from camera to encoder?**
The buffer is allocated as a dma-buf (heap or vb2 EXPBUF). The camera maps it into its
SMMU context (IOVA) and the ISP DMAs the frame in. The same dma-buf fd is imported by the
encoder, which maps the same physical pages into its own SMMU context and reads them
directly, waiting on the camera's dma_fence — no CPU copy anywhere.

**Q8. What does the DMA mask control and when does it bite for camera?**
It declares how many address bits the device's DMA can drive. Without an SMMU, a 32-bit
camera DMA can't reach buffers above 4 GB, forcing bounce buffers or failures. With an
SMMU, IOVAs are allocated within the device's addressable range, so high physical memory
is still reachable.

---

### Key takeaways
- Camera DMA uses device IOVAs translated by the SMMU; this gives scatter-gather,
  isolation (context banks/stream IDs), and wide addressing.
- Cache coherency must be maintained on capture (invalidate after DMA) or frames look
  stale despite correct addresses; vb2 normally handles it.
- Contiguity (CMA/reserved-memory) is only needed without an SMMU; modern ARM64 camera
  uses dma-sg behind the SMMU.
- dma-buf + dma-heap enable zero-copy sharing (camera→encoder→GPU→display) with fences;
  the signature bug is the SMMU context fault, decoded via IOVA + context bank + DT
  stream IDs.
