# Design: Platform Driver + DMA + Device Tree
## Level 04 | End-to-End Deep Design from Scratch

---

## 1. What Is a Platform Driver?

In embedded Linux (Android, Qualcomm SoCs), most hardware is **not on a discoverable bus** (no PCI auto-detect). The hardware address, IRQ, DMA channel, clocks are fixed in silicon — described in the **Device Tree (DT)**.

```
PCI bus  → hardware describes itself → kernel auto-binds driver
Platform → Device Tree describes HW → kernel matches driver by "compatible"
```

The kernel's **platform bus** bridges this: it reads the DT and creates `platform_device` objects that `platform_driver.probe()` is called for.

---

## 2. Device Tree Node

```dts
/* arch/arm64/boot/dts/myvendor/myboard.dts */

anil_dma_device: dma-dev@1000000 {
    compatible = "myvendor,anil-dma-device";   /* matches of_match_table */
    reg = <0x0 0x1000000 0x0 0x1000>;          /* MMIO: base=0x1000000, size=4KB */
    interrupts = <GIC_SPI 42 IRQ_TYPE_LEVEL_HIGH>;  /* IRQ line */
    dma-coherent;                               /* device is DMA coherent */
    status = "okay";
};
```

### How Kernel Uses DT

```
Boot loader → passes DT blob to kernel
Kernel parses DT → creates platform_device for each node
Driver loaded → platform_driver_register()
Kernel matches compatible string → calls probe()
```

---

## 3. `platform_driver` Structure

```c
static struct platform_driver plat_dma_driver = {
    .probe  = plat_dma_probe,   // called when device found
    .remove = plat_dma_remove,  // called when device removed
    .driver = {
        .name           = "plat_dma_dev",
        .of_match_table = plat_dma_of_match,   // DT compatible strings
    },
};

module_platform_driver(plat_dma_driver);
```

`module_platform_driver()` is a macro that expands to `module_init` + `module_exit` with `platform_driver_register/unregister`.

---

## 4. `devm_*` Resource Management

`devm_` (device-managed) functions automatically free resources when the device is removed:

| Manual | devm_ equivalent | Auto-freed when |
|--------|----------------|-----------------|
| `kzalloc` + `kfree` | `devm_kzalloc` | `pdev` removed |
| `ioremap` + `iounmap` | `devm_ioremap_resource` | `pdev` removed |
| `request_irq` + `free_irq` | `devm_request_irq` | `pdev` removed |
| `clk_get` + `clk_put` | `devm_clk_get` | `pdev` removed |

This prevents resource leaks in error paths and simplifies `remove()`.

---

## 5. MMIO — Memory-Mapped I/O

SoC peripherals are accessed via memory-mapped registers — the CPU reads/writes specific physical addresses to talk to hardware.

```c
/* Get physical address from DT "reg" property */
res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

/* Map physical address into kernel virtual address space */
p->base = devm_ioremap_resource(&pdev->dev, res);
/* Now p->base is a __iomem pointer — must use ioread32/iowrite32 */

/* Read/write HW registers */
iowrite32(value, p->base + REG_CTRL);
u32 status = ioread32(p->base + REG_STATUS);
```

### Why `ioread32` not `*((u32*)base)`?

- Prevents compiler reordering (memory barrier semantics)
- Handles big/little endian swapping correctly
- Required on architectures with strict MMIO rules (ARM, MIPS)

---

## 6. DMA — Direct Memory Access

### The Problem Without DMA

```
CPU: read 4KB from HW register byte-by-byte
     → 4096 CPU cycles busy-waiting + cache pollution
     → unacceptable for high-throughput (modem, camera, audio)
```

### DMA Solution

```
CPU: program DMA engine with:
     - source address (HW FIFO physical address)
     - dest address (kernel buffer physical address)
     - length
     - trigger DMA start

DMA engine: transfers data independently
            fires IRQ when done

CPU: handle IRQ → data already in kernel buffer → copy to user
```

### DMA Coherent Buffer

```c
dma_alloc_coherent(
    &pdev->dev,      // device for IOMMU mapping
    DMA_BUF_SIZE,    // size
    &dma_paddr,      // OUT: physical address (give to HW)
    GFP_KERNEL
);
/* Returns: dma_vaddr — CPU virtual address */
```

**Coherent** = CPU and device see the same data without explicit cache flush/invalidate. The kernel sets up the memory as uncached or uses cache coherency hardware (CCI/CCN on ARM).

### Physical vs Virtual Address

```
                    ┌────────────────┐
CPU (virtual)  ───► │  MMU  (page)   │ ───► Physical RAM
                    └────────────────┘
                           │
                    ┌──────▼─────────┐
DMA engine     ───► │  IOMMU (iova)  │ ───► Physical RAM
                    └────────────────┘

dma_paddr = IOMMU input address programmed into HW
dma_vaddr = CPU sees via MMU
```

---

## 7. IRQ from Device Tree

```c
/* Get IRQ number parsed from DT "interrupts" property */
p->irq = platform_get_irq(pdev, 0);  /* 0 = first interrupt */

/* Register handler */
devm_request_irq(&pdev->dev, p->irq, plat_dma_irq,
                 IRQF_SHARED, DRIVER_NAME, p);
```

The `GIC_SPI 42` in the DT maps to a Linux IRQ number (different from GIC number).

---

## 8. Full Data Flow: Write Path

```
User: write(fd, data, 4096)
         │
         ▼
plat_write()
├─ copy_from_user(dma_vaddr, ubuf, len)   ← user → DMA buffer
├─ hw_write(REG_DMA_ADDR_LO, paddr)       ← program DMA engine
├─ hw_write(REG_DMA_LEN, len)
├─ hw_write(REG_CTRL, DMA_START | IRQ_EN) ← kick DMA
├─ wait_event_interruptible_timeout(dma_wq, dma_done, HZ)
│                                         ← sleep until IRQ
│   [DMA engine transfers data to HW]
│
│   [IRQ fires]
│       ├─ plat_dma_irq() runs
│       ├─ clear STATUS_DONE register
│       ├─ dma_done = true
│       └─ wake_up_interruptible(&dma_wq)
│
└─ return len                             ← user sees write complete
```

---

## 9. `probe()` — Resource Acquisition Order

```
probe():
 1. devm_kzalloc()               → private data
 2. platform_set_drvdata()       → attach to pdev
 3. devm_ioremap_resource()      → MMIO mapping
 4. dma_alloc_coherent()         → DMA buffer
 5. platform_get_irq()           → IRQ number
 6. devm_request_irq()           → IRQ handler
 7. alloc_chrdev_region()        → device number
 8. cdev_init() + cdev_add()     → char device
 9. class_create()               → sysfs class
10. device_create()              → /dev entry
```

---

## 10. Device Tree Test Node (Simulation)

```dts
/* For testing without real hardware */
&soc {
    anil_dma_sim: dma@deadbeef {
        compatible = "myvendor,anil-dma-device";
        reg = <0x0 0xdeadbeef 0x0 0x100>;
        status = "disabled";   /* set "okay" to enable */
    };
};
```

---

## 11. Key Concepts Summary

| Concept | Details |
|---------|---------|
| `platform_driver` | Driver bound to DT node by `compatible` string |
| `probe()` | Called when matching DT device found |
| `devm_*` | Auto-cleanup on device removal |
| `ioremap` | Map physical MMIO → kernel virtual address |
| `ioread32/iowrite32` | Barrier-safe register access |
| `dma_alloc_coherent` | Allocate CPU+DMA visible buffer, returns paddr for HW |
| DMA paddr | Physical address programmed into DMA engine registers |
| DMA vaddr | Kernel virtual address CPU uses to read DMA result |
| `platform_get_irq` | Get IRQ number from DT "interrupts" property |
| `module_platform_driver` | Macro for platform driver module_init/exit |
