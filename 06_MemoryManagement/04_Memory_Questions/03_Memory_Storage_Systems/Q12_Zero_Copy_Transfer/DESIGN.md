# Q12 — Design a Zero-Copy Data Transfer Mechanism

---

## 1. Problem Statement

Traditional I/O (read + send, recv + write) copies data between user buffers and kernel buffers multiple times:
```
Disk → kernel page cache → user buffer → kernel socket buffer → NIC
       (copy 1)                          (copy 2)
```

Each copy is expensive: CPU cycles, cache pollution, and memory bandwidth wasted. For high-throughput systems (GPU data pipelines, NVMe over Fabrics, RDMA), eliminating copies is critical.

Design a zero-copy data transfer framework that minimizes or eliminates data copies between kernel and user space, across network interfaces, and between device DMA and application memory.

---

## 2. Requirements

### 2.1 Functional Requirements
- `sendfile()`-style zero-copy for file-to-socket transfer (no user-space copy).
- `splice()`-style pipe-based data transfer without copy.
- `mmap()` based shared memory between user and kernel.
- DMA scatter-gather: device writes directly to user pages via IOMMU.
- GPU-to-NIC zero-copy: NVIDIA GPUDirect RDMA.
- `io_uring` with registered buffers for fixed-buffer zero-copy.

### 2.2 Non-Functional Requirements
- Eliminate all CPU data copies for bulk data paths.
- Throughput: achieve ≥ 90% of theoretical PCIe bandwidth (32 GB/s for PCIe 4.0 x16).
- Latency: < 5 µs for a 4KB transfer from GPU to NIC.
- Zero additional memory allocations in the hot path.

---

## 3. Constraints & Assumptions

- Linux 6.x kernel.
- NIC with RDMA (RoCEv2) and GPU with GPUDirect RDMA (P2P PCIe).
- IOMMU enabled (Intel VT-d / AMD-Vi) for DMA safety.
- `io_uring` for userspace I/O submission.

---

## 4. Architecture Overview

```
  Traditional (with copies):
  ┌───────┐ copy1 ┌──────────┐ copy2 ┌────────┐ copy3 ┌─────┐
  │ Disk  │──────►│ Page     │──────►│ User   │──────►│ NIC │
  │       │       │ Cache    │       │ Buffer │       │ TX  │
  └───────┘       └──────────┘       └────────┘       └─────┘
                                      (3 copies, 2 mode switches)

  Zero-Copy (sendfile):
  ┌───────┐       ┌──────────┐                        ┌─────┐
  │ Disk  │──────►│ Page     │──────────────────────►│ NIC │
  │       │       │ Cache    │   sendfile():           │ TX  │
  └───────┘       └──────────┘   DMA from page cache   └─────┘
                                  (0 copies through CPU)

  GPU Zero-Copy (GPUDirect RDMA):
  ┌──────┐                ┌─────────┐                 ┌─────┐
  │ GPU  │────PCIe P2P───►│ NIC     │────Network─────►│ NIC │
  │ VRAM │                │ RDMA    │                 │ RX  │
  └──────┘                └─────────┘                 └─────┘
            (data never touches system RAM or CPU)
```

---

## 5. Core Data Structures

### 5.1 scatter-gather DMA Descriptor

```c
struct scatterlist {
    unsigned long   page_link;    /* struct page pointer + flags */
    unsigned int    offset;       /* offset within page */
    unsigned int    length;       /* segment length */
    dma_addr_t      dma_address;  /* DMA bus address (after dma_map_sg) */
    unsigned int    dma_length;
};

/* IOMMU maps a scatter list to a contiguous DMA address range */
int nents = dma_map_sg(dev, sgl, count, DMA_TO_DEVICE);
/* Now sgl[i].dma_address is the IOMMU-mapped bus address */
/* Device DMA engine reads from these addresses directly */
dma_unmap_sg(dev, sgl, count, DMA_TO_DEVICE);
```

### 5.2 io_uring Fixed Buffer Registration

```c
/* User registers pinned buffers once at setup */
struct iovec iov[N];
io_uring_register(ring_fd, IORING_REGISTER_BUFFERS, iov, N);
/*
 * Kernel: pins user pages (get_user_pages_fast), maps to IOMMU,
 * stores in io_uring context's fixed_rsrc_data.
 * On subsequent reads/writes: no page pinning overhead.
 */

/* Submit zero-copy read to registered buffer */
struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
io_uring_prep_read_fixed(sqe, fd, buf, len, offset, buf_index);
```

### 5.3 sendfile / splice pipe buffer

```c
struct pipe_buffer {
    struct page    *page;       /* the page being transferred */
    unsigned int    offset;     /* offset within page */
    unsigned int    len;        /* data length */
    const struct pipe_buf_operations *ops;
    unsigned int    flags;      /* PIPE_BUF_FLAG_CAN_MERGE, etc. */
    unsigned long   private;
};

struct pipe_inode_info {
    struct mutex        mutex;
    wait_queue_head_t   rd_wait, wr_wait;
    unsigned int        head, tail;     /* ring buffer indices */
    unsigned int        ring_size;      /* power of two */
    struct pipe_buffer *bufs;           /* ring buffer of pipe_buffer */
    unsigned int        nr_accounted;
};
```

### 5.4 MSG_ZEROCOPY Socket Notification

```c
/* Sender uses MSG_ZEROCOPY flag */
send(sock, buf, len, MSG_ZEROCOPY);
/* Kernel: pins user pages, attaches to skb frag list */
/* When NIC DMA completes: sends notification via sock error queue */

/* Poll for completion */
struct msghdr msg = {};
char cmsg_buf[CMSG_SPACE(sizeof(struct sock_extended_err))];
recvmsg(sock, &msg, MSG_ERRQUEUE);
/* Parse cmsg: contains zerocopy range [lo, hi] completed */
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 sendfile() Implementation

```c
sys_sendfile(out_fd, in_fd, offset, count):
    in_file = fdget(in_fd)
    out_file = fdget(out_fd)

    /* Check out_file supports sendpage (NIC socket) */
    if (!out_file->f_op->sendpage) goto fallback_copy

    do {
        /* Get page from page cache (no copy) */
        folio = find_get_folio(in_file->f_mapping, page_index)
        if (!folio) {
            readpage(in_file, page_index)  /* load from disk */
            folio = find_get_folio(...)
        }

        /* Send page directly to socket: no copy to user space */
        sock->ops->sendpage(sock, folio_page(folio), offset, len, flags)
        /* TCP: attaches page to sk_buff fragment list */
        /* NIC: DMA reads page directly from page cache */

        folio_put(folio)
        page_index++
    } while (remaining > 0)
```

The kernel page cache pages are referenced (refcount incremented) and attached directly to the socket `sk_buff` fragment list. The NIC DMA engine reads from the page cache pages directly — no CPU copy.

### 6.2 splice() — Kernel Pipe as Zero-Copy Channel

```c
/* Stage 1: file → pipe (no copy: move page references into pipe) */
splice_to_pipe(in_file, pipe):
    for each page in file's page cache:
        pipe_buffer->page = page  /* just a pointer assignment */
        folio_get(folio)          /* bump refcount */
        pipe_buffer->ops = &page_cache_pipe_buf_ops

/* Stage 2: pipe → socket (no copy: move page references to socket) */
splice_from_pipe(pipe, out_socket):
    for each pipe_buffer:
        sendpage(sock, pipe_buffer->page, ...)
        /* NIC DMA reads page directly */
```

`splice()` is useful for building kernel-space data pipelines: `NVMe → pipe → socket` without any CPU copy.

### 6.3 get_user_pages_fast() — Pinning User Pages for DMA

For user-space DMA (RDMA, GPU direct):
```c
/* Pin user pages — prevent them from being swapped or migrated */
int npage = get_user_pages_fast(
    user_vaddr,        /* virtual address in user space */
    npages,            /* number of pages */
    FOLL_WRITE,        /* we'll DMA-write into them */
    pages[]            /* output: array of struct page* */
);

/* Map to IOMMU for device DMA */
for (i = 0; i < npage; i++) {
    dma_addr[i] = dma_map_page(dev, pages[i], 0, PAGE_SIZE, DMA_FROM_DEVICE);
}

/* After DMA completes: */
for (i = 0; i < npage; i++) {
    dma_unmap_page(dev, dma_addr[i], PAGE_SIZE, DMA_FROM_DEVICE);
    put_page(pages[i]);   /* release pin */
}
```

**IOMMU requirement:** Without IOMMU, a compromised device could DMA to any physical address. With IOMMU, the device can only access the mapped pages.

### 6.4 GPUDirect RDMA — PCIe Peer-to-Peer

GPUDirect RDMA allows an RDMA NIC to DMA directly from/to GPU VRAM without involving system RAM:

```
GPU VRAM (BAR2 aperture) ──PCIe P2P──► NIC TX DMA
         ↑
         IOMMU maps BAR2 physical address to NIC's DMA address space
```

Implementation:
1. Register GPU VRAM pages with RDMA subsystem via `peer_memory_client` API.
2. RDMA verbs library calls `get_pages()` → GPU driver returns IOMMU-mappable physical addresses.
3. NIC programs its DMA engine with these addresses.
4. Data flows: GPU VRAM → PCIe switch → NIC TX → wire.
5. CPU is not involved; system RAM is not touched.

Kernel APIs:
```c
/* nvidia-peermem module: */
peer_mem_register(&nvidia_peer_memory);
/* Registers callbacks: get_pages, put_pages, dma_map, dma_unmap */
/* RDMA core calls these when it needs to map GPU memory for DMA */
```

### 6.5 io_uring Zero-Copy with Registered Buffers

```c
/* Registration: pin and IOMMU-map user buffers once */
io_uring_register(fd, IORING_REGISTER_BUFFERS, iovecs, n);
    → get_user_pages() for all pages in each iovec
    → dma_map_sg() for each buffer
    → store in io_uring's rsrc_data

/* Submission: submit read/write without pinning overhead */
io_uring_prep_read_fixed(sqe, fd, buf, len, offset, buf_index);
    → kernel uses pre-mapped DMA address directly
    → no get_user_pages() on the hot path
    → no IOMMU mapping overhead per I/O
```

For NVMe at 1M IOPS, the per-I/O `get_user_pages()` overhead of traditional aio adds 1 µs × 1M = 1 CPU core fully consumed by page pinning alone. Fixed buffers eliminate this entirely.

---

## 7. Trade-off Analysis

| Mechanism | Copy Count | Latency | Complexity | Use Case |
|---|---|---|---|---|
| read+send (traditional) | 2 CPU copies | High | Low | Any workload |
| sendfile() | 0 CPU copies | Low | Medium | File → socket (HTTP server) |
| splice() | 0 CPU copies | Low | High | File pipeline (tee, proxy) |
| MSG_ZEROCOPY | 0 CPU copies | Low + notification | High | Large socket sends |
| Fixed buffers (io_uring) | 0 mapping overhead | Very low | Medium | High-IOPS NVMe |
| GPUDirect RDMA | 0 copies (GPU→wire) | Ultra low | Very high | GPU ML training |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| sendfile | `mm/filemap.c`, `fs/read_write.c` | `do_sendfile()`, `generic_file_sendpage()` |
| splice | `fs/splice.c` | `do_splice()`, `splice_to_pipe()` |
| get_user_pages | `mm/gup.c` | `get_user_pages_fast()`, `pin_user_pages()` |
| DMA mapping | `kernel/dma/mapping.c` | `dma_map_sg()`, `dma_map_page()` |
| MSG_ZEROCOPY | `net/core/skbuff.c` | `skb_zerocopy_iter_stream()` |
| io_uring fixed bufs | `io_uring/rsrc.c` | `io_register_buffers()` |
| IOMMU | `drivers/iommu/iommu.c` | `iommu_map()`, `iommu_domain_alloc()` |
| GPUDirect | `drivers/infiniband/core/umem_odp.c` | `ib_umem_odp_alloc()` |
| pipe_buffer | `include/linux/pipe_fs_i.h` | `struct pipe_buffer`, `struct pipe_inode_info` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Page Pinning Failure (ENOMEM)
```bash
# get_user_pages_fast returns fewer pages than requested
# Cause: RLIMIT_MEMLOCK too low (ulimit -l)
ulimit -l unlimited   # for testing
# Or: increase /proc/sys/vm/max_map_count
```

### 9.2 IOMMU Fault (DMA to Unmapped Address)
```bash
dmesg | grep "DMAR: DRHD"   # Intel IOMMU fault reports
# "DMAR:[DMA Read] Request device ... fault addr ..."
# Cause: device DMA after dma_unmap, or use-after-free in DMA descriptor
```

### 9.3 MSG_ZEROCOPY Notification Missed
```bash
# Missed completion notification → user re-uses buffer while NIC is reading it
# Use SO_ZEROCOPY socket option + recvmsg(MSG_ERRQUEUE) polling
# Debug: ethtool -S eth0 | grep zerocopy_sent
```

---

## 10. Performance Considerations

- **TLB pressure from page pinning:** `get_user_pages()` on large buffers causes many TLB entries. Use 2MB huge pages for DMA buffers to reduce TLB entries by 512×.
- **IOMMU TLB invalidation:** After `dma_unmap()`, IOMMU must invalidate its TLB entries — expensive on systems without IOMMU TLB invalidation batching. Use `IOMMU_DOMAIN_UNMANAGED` mode for high-frequency DMA.
- **PCIe topology:** For GPUDirect RDMA, GPU and NIC must be on the same PCIe root complex, or connected via a switch with peer-to-peer routing enabled (`pcie_p2p=1`).
- **skb fragmentation:** `sendfile()` creates fragmented `sk_buff` (frags[] array). Ensure NIC supports scatter-gather DMA (all modern NICs do).

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. sendfile() mechanism: page cache → socket frag list → NIC DMA, zero CPU copies.
2. `get_user_pages()` + IOMMU mapping pipeline for user-space DMA.
3. GPUDirect RDMA: `peer_memory_client` API, PCIe P2P, no system RAM involvement.
4. io_uring fixed buffers: one-time pin+map, eliminate per-I/O overhead.
5. MSG_ZEROCOPY with completion notification via error queue.
6. IOMMU necessity: security and address translation for DMA.
7. splice() as kernel pipeline: splice(NVMe_fd, pipe) → splice(pipe, socket).
8. Huge pages for DMA: 512× TLB entry reduction.
