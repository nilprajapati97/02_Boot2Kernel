Here’s your content **cleanly reformatted into Markdown**, with **colored Mermaid diagrams** (flow + sequence) added for clarity.

---

# 🚀 How Linux Discovers Your Network Card

## e1000 Driver Registration Process

When you run:

```bash
insmod e1000.ko
```

a chain of events unfolds across **hardware, firmware, and kernel layers**.

---

# 🧱 LAYER 1: BIOS ENUMERATION (Power-On)

The BIOS scans every possible PCI address:

* `256 buses × 32 devices × 8 functions`

### Example Device:

**Intel 82540EM** → `Bus 0, Device 3, Function 0`

### Steps:

* Writes `0xFFFFFFFF` to BAR0
* Reads back: `0xFFFE0000`
* Calculates size:

  ```
  ~(0xFFFE0000) + 1 = 0x20000 (128 KB)
  ```
* Assigns:

  * Memory: `0xf0200000`
  * IRQ: `9`

👉 The NIC now responds at:

```
0xf0200000 – 0xf021FFFF
```

---

# 🧠 LAYER 2: KERNEL BOOT

Linux rescans PCI using:

```
pci_scan_root_bus()
```

### Kernel creates:

```c
struct pci_dev {
  .vendor = 0x8086,  // Intel
  .device = 0x100E,  // 82540EM
  .devfn  = 0x18,    // (3 << 3) | 0
  .irq    = 9,
  .resource[0] = {
    .start = 0xf0200000,
    .end   = 0xf021ffff,
    .flags = IORESOURCE_MEM,
  },
  .driver = NULL,    // ❗ No driver yet
}
```

---

# 🔌 LAYER 3: DRIVER REGISTRATION

The driver defines a match table:

```c
pci_device_id e1000_pci_tbl[] = {
  { PCI_VDEVICE(INTEL, 0x100E), board_82540 },
  ...
  { 0 }
};
```

### When module loads:

```
e1000_init_module()
 └─ pci_register_driver()
     └─ driver_register()
         └─ pci_bus_match()
```

---

# 🎯 LAYER 4: DEVICE MATCH

Kernel compares:

| Device           | Driver Table     |
| ---------------- | ---------------- |
| Vendor: `0x8086` | Vendor: `0x8086` |
| Device: `0x100E` | Device: `0x100E` |

✅ **MATCH FOUND**

👉 Triggers:

```
e1000_probe()
```

---

# ⚙️ LAYER 5: INSIDE `e1000_probe()`

### STEP 1: Enable Device

* BAR mask: `0x5` → `0b101`

  * BAR0 → MMIO
  * BAR2 → I/O fallback

---

### STEP 2: 🧨 Set Bus Master (CRITICAL)

* Enables **DMA**

| Without Bus Master | With Bus Master |
| ------------------ | --------------- |
| ❌ No DMA           | ✅ Full DMA      |
| ❌ No TX/RX         | ✅ Packet flow   |
| ❌ Dead NIC         | ✅ Active NIC    |

---

### STEP 3: Map Registers

```
Virtual:  0xffffc9000021FFFF
Physical: 0xf0200000 – 0xf021FFFF
```

---

### STEP 4–6:

* Allocate net device
* Reset hardware
* Read MAC address
* Register with networking stack

---

# 🔍 VERIFICATION

### PCI Config:

```bash
lspci -xxx -s 00:03.0
```

Output:

```
10: 00 00 20 f0 ...
```

👉 BAR0 = `0xf0200000` (little-endian)

---

### Driver Binding:

```bash
cat /sys/bus/pci/devices/0000:00:03.0/driver
```

Output:

```
../../drivers/e1000 ✅
```

---

# 🌐 INTERFACE ACTIVATION

```bash
ip link set enp0s3 up
```

Triggers:

```
e1000_open()
```

### Actions:

* Allocate TX/RX rings (DMA memory)
* Program NIC registers
* Register IRQ handler
* Enable interrupts
* Start packet flow 🚀

---

# 📊 FLOW DIAGRAM (Colored Mermaid)

```mermaid
flowchart TD
    A[BIOS Enumeration]:::bios --> B[Assign BAR + IRQ]:::bios
    B --> C[Linux PCI Scan]:::kernel
    C --> D[Create pci_dev Struct]:::kernel
    D --> E[Driver Loaded via insmod]:::driver
    E --> F[PCI Match Process]:::match
    F --> G[e1000_probe Called]:::probe
    G --> H[Enable Device]:::probe
    H --> I[Set Bus Master DMA]:::critical
    I --> J[Map MMIO Registers]:::probe
    J --> K[Register Net Device]:::net
    K --> L[ip link up → e1000_open]:::net

    classDef bios fill:#1f77b4,color:#fff;
    classDef kernel fill:#2ca02c,color:#fff;
    classDef driver fill:#ff7f0e,color:#fff;
    classDef match fill:#9467bd,color:#fff;
    classDef probe fill:#8c564b,color:#fff;
    classDef critical fill:#d62728,color:#fff,stroke:#000,stroke-width:2px;
    classDef net fill:#17becf,color:#fff;
```

---

# 🔄 SEQUENCE DIAGRAM (Colored)

```mermaid
sequenceDiagram
    participant BIOS
    participant Kernel
    participant PCI
    participant Driver
    participant NIC

    BIOS->>PCI: Scan all PCI devices
    BIOS->>PCI: Assign BAR + IRQ

    Kernel->>PCI: pci_scan_root_bus()
    PCI-->>Kernel: pci_dev structs

    Driver->>Kernel: insmod e1000.ko
    Kernel->>PCI: Match devices

    PCI-->>Driver: Match found (8086:100E)
    Driver->>NIC: e1000_probe()

    Driver->>NIC: Enable device
    Driver->>NIC: Set Bus Master (DMA ON)
    Driver->>NIC: Map MMIO registers

    Driver->>Kernel: Register net device

    Kernel->>Driver: ip link up
    Driver->>NIC: Setup TX/RX rings
    Driver->>NIC: Enable interrupts

    NIC-->>Kernel: Packet RX/TX via DMA
```

---

# 📚 Summary

* BIOS → discovers & assigns resources
* Kernel → builds device model
* Driver → matches & binds
* Bus Master → enables DMA (critical)
* `e1000_open()` → starts packet flow

---

If you want, I can also add:

* 🔬 register-level diagrams (TX/RX rings)
* 🧠 memory layout visuals for DMA
* ⚡ deep dive into interrupt handling (MSI/MSI-X vs INTx)

Just tell me 👍
