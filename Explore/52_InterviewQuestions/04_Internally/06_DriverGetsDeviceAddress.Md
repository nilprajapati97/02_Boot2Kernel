# 🧠 Linux Kernel Memory + `ioremap()` — Complete Flow Guide

---

# 📌 1. Big Picture

When a Linux driver talks to hardware (GPU, NIC, etc.), it uses **MMIO (Memory-Mapped I/O)**.

But:

* Devices expose **physical addresses** (PCI BARs)
* Kernel operates in **virtual address space**

👉 Bridge = `ioremap()`

---

# ⚙️ 2. What `ioremap()` Does

`ioremap()` creates:

```
Device Physical Address → Kernel Virtual Address
```

With:

* Proper **page table mappings**
* Correct **cache policy (uncached)**

---

# 🔄 3. End-to-End Flow

## STEP 1 — Driver Gets Device Address

```c
phys_addr_t phys_addr = pci_resource_start(pdev, 0); // e.g. 0xE0000000
gpu_mmio = ioremap(phys_addr, 128 * 1024 * 1024);
```

---

## STEP 2 — Kernel Reserves Virtual Range

```c
area = get_vm_area_caller(size, VM_IOREMAP, ...);
```

* No RAM allocated
* Only virtual address space reserved

Example:

```
0xFFFFC90000000000
```

---

## STEP 3 — Page Table Mapping

```c
for (...) {
    set_pte(...); // map virtual → physical
}
flush_tlb_kernel_range(...);
```

---

## STEP 4 — Cache Disabled (CRITICAL)

MMIO must bypass CPU cache:

* `_PAGE_PCD = 1` (Cache Disable)
* `_PAGE_PWT = 1` (Write Through)

---

## STEP 5 — Virtual Address Returned

```c
return (void __iomem *)virt_addr;
```

---

## STEP 6 — Driver Talks to Hardware

```c
writel(0x12345678, gpu_mmio + OFFSET);
```

---

## 🔬 Under the Hood

```
CPU writes → Virtual Address
        ↓
MMU translates → Physical Address
        ↓
Memory controller detects MMIO
        ↓
PCIe transaction
        ↓
Device receives command ✅
```

---

# 🧩 4. Virtual Address Space Layout (After Drivers Init)

![Virtual Address Space](./virtual_address_space.png)

## Key Regions

### 🟢 User Space

* 0 → ~128 TB
* Process memory, heap, stack, shared libraries

---

### 🔴 Kernel Space

#### Direct Mapping (PAGE_OFFSET)

```
Physical RAM → Directly mapped
```

Example:

```
Phys 0x00000000 → Virt 0xFFFF888000000000
```

---

### 🟣 `ioremap()` Region (MMIO)

```
Virt: 0xFFFFC90000000000 → Device Memory
```

Contains mappings for:

* GPU registers
* Network cards
* Sound cards
* APIC / IOAPIC

👉 This is where `ioremap()` places mappings

---

### 🟡 vmalloc Area

* Non-contiguous virtual allocations

### 🟠 vmemmap

* `struct page` metadata mapping

### 🔵 Kernel Text/Data

* Kernel code lives here

---

# 🧠 5. Physical RAM Layout (After Initialization)

![Physical RAM Layout](./physical_ram_layout.png)

## Key Sections

### 🔴 Reserved Regions

* BIOS
* Kernel code/data
* Initramfs
* ACPI tables

---

### 🟡 Kernel Structures

#### Page Tables

* Used by MMU

#### Zone + Per-CPU + Buddy Metadata

* Memory allocator bookkeeping

---

### 🟣 `struct page` Database

* One entry per physical page
* Example (2GB system):

  * 524,288 pages
  * 64 bytes each
  * ~32MB total

Each page tracks:

* refcount
* flags
* mapping

---

### 🟢 Free Memory (Buddy Allocator)

Zones:

* `ZONE_DMA`
* `ZONE_DMA32`

Handles allocation of pages

---

# 🔗 6. Connecting Everything

## ❌ Physical RAM ≠ Device Memory

* Physical RAM → Managed by kernel
* Device MMIO → External (PCIe devices)

---

## ✅ How They Meet

| Component   | Role                   |
| ----------- | ---------------------- |
| Page Tables | Map virtual → physical |
| Direct Map  | Maps RAM               |
| `ioremap()` | Maps devices           |
| MMU         | Translates addresses   |
| PCIe        | Routes MMIO to device  |

---

# 💡 Final Insight

> `ioremap()` is NOT memory allocation.

It is:

```
A controlled mapping layer that lets the CPU safely
and correctly talk to hardware registers in real time.
```

---

# 🧱 One-Line Mental Model

> `ioremap()` = "Give me a virtual pointer to hardware registers with zero caching."

---

# 🚀 Copy-Paste Notes for GitHub

* Save images as:

  * `virtual_address_space.png`
  * `physical_ram_layout.png`
* Place them in same directory as this markdown file

---

If you want, I can convert this into:

* 🔥 Interview cheat sheet
* 📊 Animated diagram
* 🧪 Debugging guide (how to inspect ioremap in /proc/iomem)
