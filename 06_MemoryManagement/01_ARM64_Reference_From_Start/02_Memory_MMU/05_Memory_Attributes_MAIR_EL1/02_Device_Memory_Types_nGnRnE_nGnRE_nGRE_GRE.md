# Device Memory Types: nGnRnE, nGnRE, nGRE, GRE — Deep Dive

**Category**: Memory Attributes MAIR_EL1  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. The Three Device Memory Properties

ARM64 Device memory types are characterized by three independent properties:

```
G  = Gathering
R  = Reordering
E  = Early Write Acknowledgment

Properties of "n" prefix (e.g., nG, nR, nE) = property NOT present (forbidden)
Properties without "n" prefix = property IS allowed

All four combinations:
  nGnRnE — No Gathering, No Reordering, No Early Write Ack (STRONGEST ordering)
  nGnRE  — No Gathering, No Reordering, Early Write Ack allowed
  nGRE   — No Gathering, Reordering allowed, Early Write Ack allowed
  GRE    — Gathering allowed, Reordering allowed, Early Write Ack allowed (WEAKEST)
```

---

## 2. Gathering (G / nG)

```
Gathering: Can multiple small accesses to nearby addresses be merged into 
           a single larger memory transaction?

nG = Non-Gathering (accesses MUST NOT be merged):
  Two consecutive 32-bit writes to adjacent addresses:
    DWORD 0: *(u32*)(base + 0) = 0xAA;
    DWORD 1: *(u32*)(base + 4) = 0xBB;
  
  With nG: generates EXACTLY 2 separate 32-bit transactions on the bus
    Bus transaction 1: Write 0xAA to addr+0
    Bus transaction 2: Write 0xBB to addr+4
  
  Required for: Hardware registers where size and address are significant.
    Example: DMA engine descriptor pair where first word is address,
    second word is length. If merged into 64-bit write, ordering of
    fields and timing may differ.

G = Gathering allowed (accesses MAY be merged):
  The same two 32-bit writes CAN be merged into one 64-bit transaction:
    Bus transaction 1: Write 0x_BB_AA to addr+0 (single 64-bit write)
  
  Allowed for: Framebuffer memory, write-combining regions, PCIe BARs
    with write-combining enabled.
    GPU framebuffer: scatter individual pixel writes, hardware merges
    them into full cache-line writes → far better bus utilization.

Why nG for MMIO registers:
  A device register at offset 0x100 is "Configure" and at 0x104 is "Trigger"
  They MUST be written separately in order. If gathered into one 64-bit
  transaction, the device may interpret it differently or ignite the trigger
  before the configure value is set.
```

---

## 3. Reordering (R / nR)

```
Reordering: Can the hardware (CPU store buffer, bus interconnect) reorder
            accesses to the SAME device?

nR = Non-Reordering (order of accesses to this device is preserved):
  Write to Status Register at offset 0x100
  Read from Data Register at offset 0x200
  These MUST complete in program order to/from the device.
  
  Required for: Control register sequences.
    Example: Network device:
      Write CMD register (0x00) to issue command
      Wait/Poll STATUS register (0x04) until command done
      With nR: The poll definitely observes state AFTER the command.
      Without R guarantee: Read could be seen by device BEFORE the write!

R = Reordering allowed:
  CPU/bus may complete accesses to different offsets in any order.
  Allowed for: Large memory regions with no ordering dependencies.

ARM note on nR:
  nR means no reordering of accesses to the SAME device.
  Different devices can still be accessed in different orders regardless.
  That's what DMB (Data Memory Barrier) handles at a higher level.
```

---

## 4. Early Write Acknowledgment (E / nE)

```
Early Write Acknowledgment: Can a write be acknowledged "early" (before 
the written data actually reaches the device)?

nE = No Early Write Ack (write must reach device before acknowledged):
  CPU writes to device register.
  Write travels through: CPU store buffer → L3 → interconnect → device.
  CPU is stalled (waiting for acknowledgment) until the device itself
  confirms receipt of the write.
  
  Latency: high (depends on device bus latency, can be 100s–1000s of cycles)
  
  Required for: Critical device setup registers where the write MUST
  have reached the device before the CPU can proceed.
    Example: Enabling an interrupt at an interrupt controller.
    With nE: After the write returns, the interrupt IS enabled in HW.
    With E: The write might still be in a buffer → interrupt not yet enabled.

E = Early Write Ack allowed:
  An intermediate buffer (write buffer in interconnect or SoC bridge)
  can acknowledge the write before it reaches the device.
  CPU continues immediately after the buffer accepts the write.
  The write reaches the device asynchronously later.
  
  Result: Lower write latency from CPU perspective.
  Risk: If CPU immediately reads back the written value, it may read
        the old value (write hasn't reached device yet).
  
  Allowed for: PCIe MMIO writes where PCIe root complex can buffer writes.

Practical example (UART):
  Write a byte to UART TX register:
    With nGnRnE: CPU stalls until UART acknowledges the byte write
    With nGnRE: CPU continues as soon as the SoC interconnect buffers it
    The difference: With nE, subsequent code that checks UART TX FIFO level
    is guaranteed to see the byte as consumed. With E, the byte might
    still be in transit.
```

---

## 5. When to Use Each Type

```
┌──────────────┬──────────────────────────────────────────────────────┐
│ Device Type  │ When to use                                          │
├──────────────┼──────────────────────────────────────────────────────┤
│ nGnRnE       │ MMIO registers with strict ordering requirements:     │
│ (0x00)       │  - Interrupt controllers (GIC, PLIC)                 │
│              │  - UART, SPI, I2C controllers                        │
│              │  - Timer/counter registers                           │
│              │  - Any register where write order matters to device  │
│              │  - Default for ioremap_nocache() in Linux            │
├──────────────┼──────────────────────────────────────────────────────┤
│ nGnRE        │ Device registers allowing write buffering:           │
│ (0x04)       │  - Some PCIe MMIO where root complex buffers writes  │
│              │  - USB controllers with hardware write buffering     │
│              │  - IOMMU registers (some implementations)           │
├──────────────┼──────────────────────────────────────────────────────┤
│ nGRE         │ Large register sets with no ordering dependencies:   │
│ (0x08)       │  - Sparse register spaces where order doesn't matter │
│              │  - Rarely used in practice                           │
├──────────────┼──────────────────────────────────────────────────────┤
│ GRE          │ Write-combining memory (not typical device registers)│
│ (0x0C)       │  - GPU framebuffer (write-combining enabled)         │
│              │  - PCIe BAR with Write-Combining via WC MTRRs        │
│              │  - NVLink/CXL memory BARs                            │
│              │  - Linux: ioremap_wc() uses GRE or similar          │
└──────────────┴──────────────────────────────────────────────────────┘
```

---

## 6. Linux ioremap Variants and Device Types

```c
// arch/arm64/mm/ioremap.c

// Standard MMIO (nGnRnE):
void __iomem *ioremap(phys_addr_t phys_addr, size_t size)
    → uses pgprot_device(PAGE_KERNEL)
    → AttrIndx = MT_DEVICE_nGnRnE = 0
    → MAIR slot 0 = 0x00 (nGnRnE)

// Write-combining (GRE):
void __iomem *ioremap_wc(phys_addr_t phys_addr, size_t size)
    → uses pgprot_writecombine(PAGE_KERNEL)
    → AttrIndx = MT_DEVICE_GRE = 2
    → MAIR slot 2 = 0x0C (GRE)
    // Allows gathering → GPU framebuffer writes can be merged

// Uncached (nGnRnE, explicit):
void __iomem *ioremap_nocache(phys_addr_t phys_addr, size_t size)
    → same as ioremap() on ARM64
    → AttrIndx = MT_DEVICE_nGnRnE

// DMA coherent (Non-cacheable Normal memory — NOT Device):
void *dma_alloc_coherent(...)
    → Uses MT_NORMAL_NC (AttrIndx=3, MAIR=0x44)
    → Normal Non-Cacheable (not Device!) — allows speculative reads
    → Device nGnRnE would prevent any speculation, hurting DMA buffer reads

// pgprot convenience macros:
#define pgprot_device(prot)       __pgprot_modify(prot, PTE_ATTRINDX_MASK, \
                                      PTE_ATTRINDX(MT_DEVICE_nGnRnE))
#define pgprot_writecombine(prot) __pgprot_modify(prot, PTE_ATTRINDX_MASK, \
                                      PTE_ATTRINDX(MT_DEVICE_GRE))
```

---

## 7. Common Mistake: Using Normal Memory for Device Registers

```
CRITICAL BUG: Mapping a device's MMIO registers as Normal Cacheable memory

Symptoms:
  1. Register reads return stale cached values (last value from cache, not device)
  2. Register writes are buffered in cache, never reach device until eviction
  3. Multiple CPU cores may have different views of register state
  4. Device appears to hang or malfunction intermittently

Why this happens:
  Developer maps device at VA with pgprot_kernel (Normal WB cached)
  instead of pgprot_device (Device nGnRnE)
  CPU reads from L1/L2 cache instead of going to device
  Writes never reach device until cache eviction (unpredictable timing)

Correct diagnosis:
  Check ioremap variant used
  Verify MAIR slot via PTE AttrIndx bits
  Use /proc/iomem to verify mapping, plus kernel WARN_ON for wrong pgprot

The reverse mistake (Device for DMA buffers):
  Using nGnRnE for Normal DMA buffers → prevents CPU speculation
  DMA buffer reads cannot be prefetched → severe performance loss
  Use MT_NORMAL_NC for DMA coherent buffers, not Device!
```

---

## 8. Interview Questions & Answers

**Q1: Explain the difference between nGnRnE and GRE device memory types.**

`nGnRnE` is the strictest device memory type with three restrictions: *non-Gathering* (each CPU access generates a separate bus transaction, no merging), *non-Reordering* (accesses reach the device in program order), and *no Early Write Acknowledgment* (CPU stalls until the device itself confirms receipt). It's used for MMIO registers that require exact sequencing, like interrupt controllers and UART/SPI controllers.

`GRE` is the most relaxed device type: *Gathering* (adjacent writes may be merged into larger transactions), *Reordering* (hardware can reorder accesses for efficiency), and *Early Write Acknowledgment* (an intermediate buffer acknowledges writes, not the device itself). It's used for write-combining regions like GPU framebuffers where merging multiple small writes into cache-line-sized transfers drastically improves throughput.

**Q2: Why must device MMIO registers NOT be mapped as Normal Cacheable memory?**

Normal Cacheable memory allows the CPU to: (1) read from L1/L2 cache instead of the device — reads return stale cached values rather than current device state; (2) write to cache first and defer DRAM/device writes until eviction — register writes may never reach the device in time; (3) reorder accesses for cache efficiency — sequence-sensitive register operations execute in wrong order; (4) multiple CPUs may hold different cached values of the same register address — incoherent views of device state. Device nGnRnE prevents all these issues by ensuring every access reaches the actual device in order, with no caching or merging.

---

## 9. Quick Reference

| Type | MAIR byte | G? | R? | E? | Typical use |
|---|---|---|---|---|---|
| nGnRnE | 0x00 | No | No | No | MMIO registers (default) |
| nGnRE | 0x04 | No | No | Yes | PCIe MMIO with write buffers |
| nGRE | 0x08 | No | Yes | Yes | Sparse register spaces |
| GRE | 0x0C | Yes | Yes | Yes | Framebuffer, write-combining |

| Property | Meaning | Required when |
|---|---|---|
| nG | No gathering (no merging) | Register size/count matters to device |
| nR | No reordering | Sequential register protocol required |
| nE | No early write ack | Write must reach device before proceeding |
