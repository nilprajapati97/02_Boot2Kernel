# Q19 — Design a High-Throughput Network Driver

---

## 1. Problem Statement

A high-throughput network driver (100Gbps+) cannot use the traditional Linux network stack without careful design — the default path has per-packet costs that saturate a CPU core at ~1–10 Gbps. At 100 Gbps with 64-byte packets, the driver must process ~148 million packets per second.

Design a network driver that achieves wire-rate throughput using:
- NAPI (New API) for interrupt mitigation and poll-mode processing.
- Multi-queue TX/RX with per-CPU queue assignment.
- XDP (eXpress Data Path) for kernel-bypass packet processing.
- GRO (Generic Receive Offload) and TSO/GSO to amortize per-packet overhead.
- Zero-copy RX via page flipping (driver recycles pages without copy).

---

## 2. Requirements

### 2.1 Functional Requirements
- 100Gbps wire-rate TX and RX for 1500-byte MTU (64-byte minimum frame).
- Multi-queue: ≥ 64 TX queues and 64 RX queues.
- XDP programs: execute eBPF at receive time before SKB allocation.
- RSS (Receive Side Scaling): hash-based flow steering to per-CPU queues.
- NAPI polling: adaptive interrupt coalescing.
- TCP segmentation offload (TSO) and large receive offload (LRO/GRO).

### 2.2 Non-Functional Requirements
- RX latency (wire to application): < 10 µs.
- CPU utilization: < 50% per core at 10 Mpps on that core's queue.
- Zero packet drops under sustained 100Gbps load (given sufficient CPU cores).

---

## 3. Constraints & Assumptions

- 100GbE NIC with 64 TX + 64 RX hardware queues.
- Linux 6.x kernel with XDP, NAPI, ethtool APIs.
- Each RX queue has an independent interrupt vector (MSI-X).
- Interrupt-to-CPU mapping: RSS steers flows to specific CPUs.

---

## 4. Architecture Overview

```
  Wire (100GbE)
      │
      ▼
  NIC Hardware
  ┌──────────────────────────────────────────────┐
  │  RX DMA Ring (HW queue 0..63)               │
  │  Each ring: descriptor ring + data buffers  │
  │  RSS hash → steer to queue N                │
  └───────────────┬──────────────────────────────┘
                  │ MSI-X interrupt per queue
                  ▼
  ┌──────────────────────────────────────────────┐
  │  NAPI Poll (softirq, per-CPU)               │
  │  ┌─────────────────────────────────────────┐ │
  │  │  XDP Program (eBPF, runs BEFORE SKB)    │ │
  │  │  XDP_DROP / XDP_TX / XDP_REDIRECT /    │ │
  │  │  XDP_PASS (→ continue to network stack) │ │
  │  └─────────────────────────────────────────┘ │
  │  ┌─────────────────────────────────────────┐ │
  │  │  GRO (coalesce TCP segments)            │ │
  │  │  → larger virtual packets to TCP stack  │ │
  │  └─────────────────────────────────────────┘ │
  └───────────────┬──────────────────────────────┘
                  ▼
  TCP/IP Network Stack → Socket receive buffer → Application
```

---

## 5. Core Data Structures

### 5.1 RX Descriptor Ring

```c
/* Hardware-format RX descriptor (NIC-specific) */
struct my_rx_desc {
    __le64  buf_addr;    /* DMA address of receive buffer (page fragment) */
    __le16  length;      /* filled by NIC: actual received data length */
    __le16  vlan_tag;    /* if VLAN offload enabled */
    __le32  status;      /* MY_RX_STATUS_DD = descriptor done (NIC wrote data) */
    __le32  errors;
    __le32  rss_hash;    /* RSS flow hash */
    __le16  queue_index; /* which NIC queue received this */
    __le16  pkt_type;    /* IP/TCP/UDP classification by NIC */
};

struct my_rx_ring {
    struct my_rx_desc  *desc;        /* descriptor ring (coherent DMA memory) */
    dma_addr_t          desc_dma;
    struct my_rx_buf   *rx_buf;      /* per-descriptor SW state */
    u32                 count;       /* ring depth (power of 2) */
    u32                 next_to_use; /* driver fills buffers here */
    u32                 next_to_clean; /* driver processes completions here */
    void __iomem       *tail;        /* RDT register: write to notify NIC of new buffers */
    struct napi_struct  napi;        /* NAPI instance for this queue */
    struct xdp_rxq_info xdp_rxq;    /* XDP RX queue info */
};

struct my_rx_buf {
    struct page   *page;       /* receive page */
    dma_addr_t     dma;        /* DMA mapping of page */
    unsigned int   page_offset; /* current offset in page */
    u32            pagecnt_bias; /* page reference count optimization */
};
```

### 5.2 TX Descriptor Ring + SKB Tracking

```c
struct my_tx_ring {
    struct my_tx_desc  *desc;
    dma_addr_t          desc_dma;
    struct my_tx_buf   *tx_buf;   /* per-descriptor SW: pointers to free */
    u32                 count;
    u32                 next_to_use;
    u32                 next_to_clean;
    void __iomem       *tail;     /* TDT register: write to kick NIC */
    struct netdev_queue *txq;     /* netdev TX queue handle */
};

struct my_tx_buf {
    struct sk_buff *skb;       /* NULL for frags, set on last descriptor */
    dma_addr_t      dma;
    u32             len;
    bool            mapped_as_page;
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 NAPI — Interrupt Mitigation at 100 Gbps

Without NAPI, each packet fires an interrupt: 148M interrupts/sec at 64B/100Gbps = impossible.

NAPI mixes interrupts with polling:
```c
/* RX interrupt handler: schedule NAPI poll, disable interrupt */
irqreturn_t my_rx_irq(int irq, void *data)
{
    struct my_rx_ring *ring = data;

    /* Disable this queue's interrupt — will be re-enabled by NAPI */
    my_irq_disable(ring);

    /* Schedule NAPI poll on this CPU */
    napi_schedule(&ring->napi);

    return IRQ_HANDLED;
}

/* NAPI poll: process up to 'budget' packets */
int my_napi_poll(struct napi_struct *napi, int budget)
{
    struct my_rx_ring *ring = container_of(napi, struct my_rx_ring, napi);
    int cleaned = 0;

    while (cleaned < budget && ring_has_completions(ring)) {
        struct my_rx_desc *desc = &ring->desc[ring->next_to_clean];

        if (!(desc->status & MY_RX_STATUS_DD))
            break;  /* NIC has not finished writing this descriptor */

        /* Run XDP program (if attached) BEFORE SKB allocation */
        if (ring->xdp_prog) {
            xdp_result = my_run_xdp(ring, desc);
            if (xdp_result != XDP_PASS)
                goto next;  /* XDP handled: drop, redirect, or TX */
        }

        /* Allocate SKB and pass to network stack */
        skb = my_build_skb(ring, desc);
        napi_gro_receive(napi, skb);  /* GRO coalescing */

    next:
        my_advance_ring(ring);
        cleaned++;
    }

    /* Refill RX ring with new page buffers */
    my_alloc_rx_buffers(ring, cleaned);

    if (cleaned < budget) {
        /* No more work: exit poll, re-enable interrupt */
        napi_complete_done(napi, cleaned);
        my_irq_enable(ring);
    }

    return cleaned;
}
```

### 6.2 XDP — Zero-Overhead Early Packet Drop / Redirect

XDP attaches a BPF program to the RX path before any SKB allocation:

```c
/* Driver invokes XDP program */
static int my_run_xdp(struct my_rx_ring *ring, struct my_rx_desc *desc)
{
    struct xdp_buff xdp;
    u32 act;

    xdp_init_buff(&xdp, PAGE_SIZE, &ring->xdp_rxq);
    xdp_prepare_buff(&xdp, page_addr, offset, pkt_len, false);

    act = bpf_prog_run_xdp(ring->xdp_prog, &xdp);

    switch (act) {
    case XDP_PASS:      return XDP_PASS;      /* continue to network stack */
    case XDP_DROP:      /* drop: recycle page, no SKB */
        my_rx_recycle(ring, desc);
        return XDP_DROP;
    case XDP_TX:        /* transmit out same interface (e.g., reflect) */
        my_xdp_xmit(ring, &xdp);
        return XDP_TX;
    case XDP_REDIRECT:  /* redirect to another NIC, CPU, AF_XDP socket */
        xdp_do_redirect(ring->netdev, &xdp, ring->xdp_prog);
        return XDP_REDIRECT;
    default:
        bpf_warn_invalid_xdp_action(ring->netdev, ring->xdp_prog, act);
        return XDP_ABORTED;
    }
}
```

**XDP advantage:** 40+ Mpps achievable for XDP_DROP/XDP_TX vs ~10 Mpps through the full network stack.

### 6.3 Page Recycling — Zero-Copy RX

Instead of allocating a new page per packet, reuse the same page for multiple receives:

```c
/* Each 4KB page can hold two 2KB RX buffers (half-page technique) */
/* When one half is received, use the other half for next buffer */

struct my_rx_buf {
    struct page *page;
    u32          page_offset;  /* 0 or PAGE_SIZE/2 */
    u32          pagecnt_bias; /* expected page refcount */
};

bool my_can_reuse_rx_page(struct my_rx_buf *rx_buf)
{
    unsigned int pagecnt = page_count(rx_buf->page) - rx_buf->pagecnt_bias;

    /* Page is still referenced by SKB frags: cannot reuse */
    if (unlikely(pagecnt != 1))
        return false;

    /* Flip to other half of page */
    rx_buf->page_offset ^= (PAGE_SIZE / 2);
    rx_buf->pagecnt_bias++;
    return true;
}
```

### 6.4 GRO — Reducing SKB Processing Overhead

GRO coalesces multiple TCP segments with contiguous sequence numbers into one large SKB:

```
Without GRO: 10 × 1460B packets → 10 SKBs → 10 × tcp_rcv() calls
With GRO:    10 × 1460B packets → coalesced to 1 × 14600B SKB → 1 × tcp_rcv() call
```

```c
/* Driver passes SKB to GRO engine */
napi_gro_receive(napi, skb);

/* GRO engine: */
/*   Check if skb matches an existing GRO flow (same src/dst IP, port) */
/*   If yes: append data, update TCP header */
/*   If no: flush accumulated flow to stack, start new flow */
/*   GRO flush timeout: 2 ms (avoids excessive coalescing latency) */
```

### 6.5 RSS — Receive Side Scaling for Multi-Queue

RSS distributes incoming flows across RX queues based on packet hash:

```
NIC computes: hash = Toeplitz(src_IP, dst_IP, src_port, dst_port)
Queue = hash % num_queues

The indirection table (RETA) maps hash buckets to queues:
    reta[hash % 128] = queue_id

Driver programs RETA at init and allows ethtool to reconfigure:
    ethtool -X eth0 equal 64   # distribute evenly across 64 queues
```

---

## 7. Trade-off Analysis

| Mechanism | Throughput Gain | Latency Cost | CPU Overhead |
|---|---|---|---|
| NAPI batching | High (10-100x) | +0.1–1 ms | Lower (fewer interrupts) |
| XDP (pass-through) | +20-40% | None | Lower (no SKB alloc) |
| GRO | High (10-100x SKB reduction) | +2 ms max | Lower (fewer tcp_rcv calls) |
| TSO | High TX (1 SKB = 64KB) | None | Lower (fewer tx_map_sg calls) |
| Page recycling | ~5% | None | Lower (no page alloc) |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| NAPI | `net/core/dev.c` | `napi_schedule()`, `napi_complete_done()`, `napi_gro_receive()` |
| XDP | `net/core/filter.c` | `bpf_prog_run_xdp()`, `xdp_do_redirect()` |
| GRO | `net/core/gro.c` | `napi_gro_receive()`, `dev_gro_receive()` |
| SKB allocation | `net/core/skbuff.c` | `build_skb()`, `napi_build_skb()` |
| Ring buffer page | `drivers/net/ethernet/intel/i40e/i40e_txrx.c` | `i40e_alloc_rx_buffers()` |
| AF_XDP | `net/xdp/xsk.c` | `xsk_rcv()`, `xsk_umem_consume_tx()` |
| ethtool RSS | `net/ethtool/ioctl.c` | `ethtool_set_rxfh()` |
| TSO | `net/ipv4/tcp_offload.c` | `tcp_gso_segment()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Packet Drops at High Rate
```bash
ethtool -S eth0 | grep -i "drop\|miss\|error"  # driver stats
netstat -s | grep "receive buffer errors"       # socket buffer drops
# Increase RX ring size:
ethtool -G eth0 rx 4096
```

### 9.2 RX Interrupt Affinity Imbalance
```bash
cat /proc/interrupts | grep eth0    # verify IRQ → CPU mapping
ethtool -l eth0                     # queue count
ethtool -L eth0 combined 64        # set 64 queues
# Configure RSS IRQ affinity via irqbalance or manual cpumask
```

### 9.3 XDP Program Not Executing
```bash
ip link show eth0 | grep xdp   # shows attached XDP program
bpftool prog list               # list all loaded BPF programs
bpftool net show                # show XDP attachments
```

---

## 10. Performance Considerations

- **Interrupt coalescing (ethtool -C):** Increase `rx-usecs` and `rx-frames` to reduce interrupt rate. Tradeoff: higher latency vs lower CPU.
- **NUMA affinity:** NIC's IRQ → CPU should be on the same NUMA node as PCIe root complex. Cross-NUMA interrupt processing adds ~200 ns.
- **Busy polling:** `SO_BUSY_POLL` / `epoll` with `EPOLLET` — application polls socket in userspace, skips interrupt entirely.
- **AF_XDP zero-copy:** Userspace application shares UMEM (user-space memory) with NIC. Packets DMA'd directly into application memory without any copy.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. NAPI: single interrupt → N packets processed in poll. `napi_schedule()` + `napi_complete_done()`.
2. XDP path: BPF program executes before `build_skb()` — saves 200+ ns per packet for XDP_DROP/TX.
3. Page recycling: half-page flip avoids page allocator in hot path.
4. GRO: TCP coalescing reduces per-packet overhead by ~10x for streaming traffic.
5. RSS + MSI-X: each RX queue → one CPU → no lock contention in RX path.
6. AF_XDP UMEM: true zero-copy where application shares DMA memory with NIC.
7. TSO: kernel sends one 64KB SKB, NIC splits into MTU-sized frames — saves 43 tx_map() calls.
8. `ethtool -S`: first tool to reach for packet drop debugging.
