# Q17 — Design a PCIe Device Communication Framework

---

## 1. Problem Statement

PCIe is the primary interconnect for high-performance devices (GPUs, NVMe SSDs, NICs, FPGAs). A PCIe communication framework must:
- Enumerate and configure PCIe devices at boot (BAR mapping, capability scanning).
- Provide MMIO-based register access for data-path critical operations.
- Set up MSI-X for per-queue interrupt routing with CPU affinity.
- Handle DMA-based bulk data transfer (producer-consumer rings).
- Implement error detection and recovery (AER — Advanced Error Reporting).
- Support SR-IOV for virtualization (multiple VFs per PF).

Design the complete framework from PCIe enumeration through production data-path operation and fault recovery.

---

## 2. Requirements

### 2.1 Functional Requirements
- PCIe device discovery, BAR resource allocation, MMIO mapping.
- MSI-X interrupt allocation with per-CPU affinity routing.
- DMA engine setup: descriptor ring, doorbell, completion queue.
- Command queue implementation (submission ring + completion ring).
- Error handling: correctable, uncorrectable, fatal PCIe errors (AER).
- SR-IOV: create and manage Virtual Functions for guest VMs.

### 2.2 Non-Functional Requirements
- MMIO access latency: < 1 µs for 32-bit register read.
- Interrupt latency (MSI-X to softirq): < 5 µs.
- DMA throughput: > 90% of theoretical PCIe bandwidth.
- Error recovery: soft reset of device within 100 ms after AER fatal error.

---

## 3. Constraints & Assumptions

- PCIe 4.0 x16 (64 GB/s theoretical).
- x86-64 with Intel IOMMU (VT-d).
- Linux PCI subsystem (`drivers/pci/`).
- Device is a hypothetical high-bandwidth accelerator (NVIDIA A100 equivalent).

---

## 4. Architecture Overview

```
  PCIe Bus Topology:
  ┌──────────────────────────────────────────────────┐
  │  CPU (Root Complex)                              │
  │  ├── Root Port 0                                │
  │  │     └── PCIe Switch                          │
  │  │           ├── Device 0 (GPU): BDF 03:00.0    │
  │  │           └── Device 1 (NIC): BDF 04:00.0    │
  │  └── Root Port 1                                │
  │        └── NVMe SSD: BDF 01:00.0                │
  └──────────────────────────────────────────────────┘

  Device BAR Map:
  BAR0: Control registers (256KB, MMIO, non-prefetchable)
  BAR2: Doorbell registers (64KB, MMIO, non-prefetchable)
  BAR4: VRAM aperture (16GB, prefetchable, 64-bit)

  Kernel Driver:
  pci_probe() → ioremap(BAR0) → pci_alloc_irq_vectors() →
  alloc_descriptor_rings() → request_irq() → register with subsystem
```

---

## 5. Core Data Structures

### 5.1 Device State

```c
struct my_pcie_dev {
    struct pci_dev          *pdev;           /* PCI device handle */

    /* BAR mappings */
    void __iomem            *bar0;           /* Control registers base */
    void __iomem            *bar2;           /* Doorbell registers base */
    phys_addr_t              bar4_phys;      /* VRAM aperture physical */
    resource_size_t          bar4_len;       /* VRAM aperture size */

    /* MSI-X */
    int                      num_vectors;    /* allocated MSI-X vectors */
    struct msix_entry       *msix_entries;   /* vector → IRQ mapping */

    /* Submission/completion queues */
    struct my_sq            *sq;             /* submission queues (one per engine) */
    struct my_cq            *cq;             /* completion queues */
    int                      num_queues;

    /* AER */
    pci_ers_result_t         last_aer_result;
    bool                     in_reset;

    /* DMA */
    struct dma_pool         *small_dma_pool; /* descriptors < 4KB */
};
```

### 5.2 Submission Queue + Completion Queue (like NVMe SQ/CQ)

```c
struct my_sq {
    void                    *virt;       /* CPU-writable, device-readable */
    dma_addr_t               dma;        /* DMA address for device */
    u32                      depth;      /* ring capacity */
    u32                      head;       /* device updates via DMA */
    u32                      tail;       /* driver updates */
    void __iomem            *doorbell;   /* BAR2 doorbell register */
    spinlock_t               lock;
};

struct my_cq {
    void                    *virt;       /* device-writable (completion entries) */
    dma_addr_t               dma;
    u32                      depth;
    u32                      head;       /* driver processes completions here */
    u16                      phase;      /* phase bit: alternates 0/1 each wrap */
    int                      irq;        /* MSI-X IRQ servicing this CQ */
};

struct my_completion_entry {
    u64  status;      /* command-specific result */
    u16  sq_head;     /* updated SQ head (device consumed this many entries) */
    u16  sq_id;       /* which SQ this completion is for */
    u16  cmd_id;      /* command identifier */
    u16  status_code; /* 0 = success */
    u8   phase;       /* matches current CQ phase bit when valid */
};
```

### 5.3 PCIe Capability Structures (from config space)

```c
/* PCIe capabilities found via pci_find_capability() */

/* MSI-X capability */
struct msix_cap {
    u8   cap_id;          /* PCI_CAP_ID_MSIX */
    u16  msg_ctrl;        /* [10:0] = table size - 1, [15] = enable */
    u32  table_offset;    /* [2:0] = BAR index, [31:3] = table offset */
    u32  pba_offset;      /* pending bit array offset */
};

/* PCIe Link Capability */
struct pcie_link_cap {
    u32  max_link_speed;  /* 1=2.5GT/s, 2=5GT/s, 3=8GT/s, 4=16GT/s, 5=32GT/s */
    u32  max_link_width;  /* x1, x2, x4, x8, x16 */
};

/* AER (Advanced Error Reporting) Capability */
struct pcie_aer_cap {
    u32  uncor_status;    /* uncorrectable error status register */
    u32  uncor_mask;      /* uncorrectable error mask */
    u32  uncor_severity;  /* which uncorrectable errors are fatal */
    u32  cor_status;      /* correctable error status */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 PCIe Device Probing

```c
static int my_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    /* 1. Enable device and bus mastering (allow DMA) */
    ret = pci_enable_device(pdev);
    pci_set_master(pdev);

    /* 2. Configure DMA mask */
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

    /* 3. Request and map BARs */
    ret = pci_request_regions(pdev, DRV_NAME);
    dev->bar0 = pci_ioremap_bar(pdev, 0);   /* ioremap for MMIO access */
    dev->bar2 = pci_ioremap_bar(pdev, 2);

    /* 4. Allocate MSI-X vectors */
    dev->num_vectors = min(MAX_QUEUES, num_online_cpus());
    ret = pci_alloc_irq_vectors(pdev, 1, dev->num_vectors, PCI_IRQ_MSIX);
    dev->num_vectors = ret;  /* actual allocated count */

    /* 5. Allocate SQ/CQ rings (coherent DMA memory) */
    for (i = 0; i < dev->num_queues; i++) {
        dev->sq[i].virt = dma_alloc_coherent(&pdev->dev,
                              SQ_DEPTH * sizeof(struct my_cmd),
                              &dev->sq[i].dma, GFP_KERNEL);
        dev->cq[i].virt = dma_alloc_coherent(&pdev->dev,
                              CQ_DEPTH * sizeof(struct my_completion_entry),
                              &dev->cq[i].dma, GFP_KERNEL);
    }

    /* 6. Register interrupt handlers */
    for (i = 0; i < dev->num_vectors; i++) {
        irq = pci_irq_vector(pdev, i);
        request_irq(irq, my_cq_irq_handler, 0, DRV_NAME, &dev->cq[i]);
        /* Set CPU affinity: IRQ i → CPU i */
        cpumask_set_cpu(i % num_online_cpus(), &mask);
        irq_set_affinity_hint(irq, &mask);
    }

    /* 7. Initialize device queues (write BAR0 registers) */
    my_device_init_queues(dev);

    return 0;
}
```

### 6.2 MMIO Register Access

```c
/* All MMIO reads/writes must use accessor functions to ensure proper ordering */

static inline u32 my_reg_read32(struct my_pcie_dev *dev, u32 offset)
{
    return readl(dev->bar0 + offset);  /* ensures load/store ordering, volatile */
}

static inline void my_reg_write32(struct my_pcie_dev *dev, u32 offset, u32 val)
{
    writel(val, dev->bar0 + offset);   /* wmb() equivalent via writel() semantics */
}

/* Doorbell write (critical path — must be fast) */
static inline void my_doorbell_ring(struct my_sq *sq, u32 tail)
{
    /* MMIO write to doorbell register in BAR2 */
    /* x86: writel() includes compiler barrier + store-store ordering */
    writel(tail, sq->doorbell);
}
```

### 6.3 Command Submission (Hot Path)

```c
int my_submit_command(struct my_pcie_dev *dev, int sq_id,
                      struct my_cmd *cmd)
{
    struct my_sq *sq = &dev->sq[sq_id];
    u32 tail;

    spin_lock(&sq->lock);

    /* Check queue full: tail + 1 == head */
    tail = sq->tail;
    if ((tail + 1) % sq->depth == sq->head) {
        spin_unlock(&sq->lock);
        return -EBUSY;  /* queue full */
    }

    /* Copy command into ring slot */
    cmd->cmd_id = sq->tail;
    memcpy(sq->virt + tail * CMD_SIZE, cmd, CMD_SIZE);

    /* Update tail */
    sq->tail = (tail + 1) % sq->depth;

    /* Ring doorbell: device sees new tail, fetches command */
    my_doorbell_ring(sq, sq->tail);

    spin_unlock(&sq->lock);
    return tail;  /* return command ID for completion tracking */
}
```

### 6.4 MSI-X Interrupt Routing and CPU Affinity

MSI-X allows each interrupt vector to target a specific CPU APIC:

```
MSI-X table entry:
    [63:20] Message Address = APIC base (0xFEE00000) | Destination ID (CPU APIC ID)
    [19:12] Extended Destination
    [31:0]  Message Data = vector number | trigger mode | level/edge

Setup per-queue affinity:
    For queue i targeting CPU j:
        msg_addr = 0xFEE00000 | (apic_id_of_cpu_j << 12)
        msg_data = VECTOR_BASE + i
        Write to MSIX_TABLE[i].msg_addr, MSIX_TABLE[i].msg_data
```

This ensures completion interrupts for queue 0 always arrive on CPU 0, completion for queue 1 always on CPU 1 — eliminating cross-CPU cache invalidation in the completion handler.

### 6.5 AER Error Handling

```c
static struct pci_error_handlers my_err_handler = {
    .error_detected = my_aer_error_detected,
    .mmio_enabled   = my_aer_mmio_enabled,
    .slot_reset     = my_aer_slot_reset,
    .resume         = my_aer_resume,
};

/* Called when PCIe AER detects uncorrectable error */
pci_ers_result_t my_aer_error_detected(struct pci_dev *pdev,
                                        pci_channel_state_t state)
{
    if (state == pci_channel_io_frozen) {
        /* Device is frozen: disable I/O, stop DMA */
        pci_disable_device(pdev);
        return PCI_ERS_RESULT_NEED_RESET;
    }
    if (state == pci_channel_io_perm_failure) {
        /* Unrecoverable: remove device */
        return PCI_ERS_RESULT_DISCONNECT;
    }
    return PCI_ERS_RESULT_CAN_RECOVER;
}

/* After FLR (Function Level Reset) by PCI core */
pci_ers_result_t my_aer_slot_reset(struct pci_dev *pdev)
{
    pci_restore_state(pdev);     /* restore config space from saved state */
    pci_enable_device(pdev);
    pci_set_master(pdev);
    my_device_reinit(dev);       /* re-initialize queues, re-enable interrupts */
    return PCI_ERS_RESULT_RECOVERED;
}
```

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| MSI-X | Yes | Legacy INTx / MSI | MSI-X: per-queue vector, CPU affinity, no sharing |
| Coherent DMA for rings | Yes | Streaming DMA | Ring entries are small, frequently accessed by CPU → coherent avoids sync |
| Doorbell via MMIO BAR2 | Yes | Doorbell via config space | BAR2 can be mapped to userspace; config space requires kernel |
| One CQ per CPU | Yes | Single shared CQ | Eliminates CQ lock contention at high queue depths |
| AER + FLR recovery | Yes | Reboot on error | FLR is transparent: only affected device resets, not whole system |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| PCI device probe | `drivers/pci/pci.c` | `pci_enable_device()`, `pci_set_master()` |
| BAR mapping | `include/linux/pci.h` | `pci_ioremap_bar()`, `pci_resource_start()` |
| MSI-X allocation | `drivers/pci/msi/msi.c` | `pci_alloc_irq_vectors()`, `pci_irq_vector()` |
| IRQ affinity | `kernel/irq/affinity.c` | `irq_set_affinity_hint()`, `irq_calc_affinity_vectors()` |
| AER | `drivers/pci/pcie/aer.c` | `pcie_do_recovery()`, `pci_cleanup_aer_uncorrect_error_status()` |
| MMIO accessors | `include/asm-generic/io.h` | `readl()`, `writel()`, `ioread32()`, `iowrite32()` |
| SR-IOV | `drivers/pci/iov.c` | `pci_enable_sriov()`, `pci_num_vf()` |
| DMA mask | `include/linux/dma-mapping.h` | `dma_set_mask_and_coherent()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Device Not Detected After Boot
```bash
lspci -vvv | grep -A 30 "10de:"   # verify device appears in PCI scan
dmesg | grep "pci 0000:"           # check BDF, BAR allocation, IRQ assignment
# Common cause: PCIe slot power issue, BIOS IOMMU conflict
```

### 9.2 MMIO Read Returns 0xFFFFFFFF
```bash
# All-ones read = device not responding (PCIe config space error)
# dmesg: look for "pci error" or AER messages
# Check: pci_disable_device() accidentally called?
cat /sys/bus/pci/devices/0000:03:00.0/enable   # should be "1"
```

### 9.3 MSI-X Not Working (using fallback INTx)
```bash
cat /proc/interrupts | grep "my_pcie_dev"  # check IRQ type (PCI-MSI vs PCI-MSI-X)
# PCI_IRQ_MSIX flag in pci_alloc_irq_vectors() and MSI-X table not disabled in BAR
```

### 9.4 Performance Below Expected
```bash
# Check link width and speed
lspci -vvv | grep "LnkSta"   # "LnkSta: Speed 16GT/s, Width x16" is PCIe 4.0 x16
# If "Width x1": check physical slot, CPU PCIe bifurcation settings
```

---

## 10. Performance Considerations

- **Doorbell batching:** Avoid ringing doorbell after every command. Accumulate multiple commands, ring once — reduces MMIO write overhead.
- **Polling vs interrupt:** For < 10 µs completion, poll CQ in tight loop on dedicated CPU rather than using MSI-X interrupt.
- **PCIe read latency:** MMIO reads cross PCIe — ~500 ns. Minimize reads in hot path. Use write-only doorbell pattern (no read-back).
- **DMA alignment:** PCIe TLP maximum payload (512B default, 4KB with large BAR). Align DMA transfers to TLP boundaries for full-width transfers.
- **NUMA affinity:** PCIe root complex is NUMA-local to specific CPUs. Submit I/O from NUMA-local CPUs to minimize PCIe path latency.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. BAR enumeration and ioremap — why BAR0 is non-prefetchable (registers have side effects).
2. MSI-X table: per-vector CPU affinity via APIC message address encoding.
3. SQ/CQ rings with coherent DMA memory — device and CPU share without explicit cache flush.
4. Doorbell register in BAR2 — can be mapped to userspace for zero-syscall submission.
5. AER: error_detected → slot_reset → resume callback sequence.
6. FLR (Function Level Reset): resets device without PCIe bus reset — less disruptive.
7. SR-IOV: PF creates VFs; each VF has its own BDF, BARs, MSI-X vectors — full hardware isolation for VMs.
8. MMIO read stall: 500 ns PCIe latency → avoid reads in hot path.
