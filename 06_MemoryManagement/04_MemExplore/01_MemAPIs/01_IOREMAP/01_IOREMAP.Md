Perfect 👌 — this is a **core concept in Linux kernel driver development**. Let’s go step by step into **`ioremap()` and the I/O read/write APIs** used by the kernel.

---

# 🔹 1. Why `ioremap()` is needed?

* Device registers (MMIO – Memory Mapped I/O) are located at **physical addresses** (from SoC datasheet, e.g., `0x48060000` for I²C on OMAP).
* Kernel runs in **virtual address space** → it cannot directly dereference physical addresses.
* `ioremap()` provides a mapping from **physical address → kernel virtual address**, so the driver can safely access the registers.

---

# 🔹 2. `ioremap()` API

```c
void __iomem *ioremap(resource_size_t phys_addr, unsigned long size);
void iounmap(void __iomem *addr);
```

* **`phys_addr`** → starting physical address of device registers.
* **`size`** → size of register space to map.
* Returns: kernel virtual address with `__iomem` annotation.
* Must be released with `iounmap()`.

👉 Variants:

* `ioremap()` → normal I/O mapping (uncached).
* `ioremap_nocache()` → legacy (same as ioremap).
* `devm_ioremap_resource(dev, res)` → managed API (recommended, auto-unmaps).

---

# 🔹 3. Read/Write APIs after mapping

Once you have a virtual address from `ioremap()`, **you must not use normal pointer dereference (`*ptr = value`)**.
Instead, kernel provides **I/O accessor APIs**:

### 32-bit and 8/16/64-bit variants:

```c
u8   ioread8(void __iomem *addr);
u16  ioread16(void __iomem *addr);
u32  ioread32(void __iomem *addr);
u64  ioread64(void __iomem *addr);

void iowrite8(u8 value, void __iomem *addr);
void iowrite16(u16 value, void __iomem *addr);
void iowrite32(u32 value, void __iomem *addr);
void iowrite64(u64 value, void __iomem *addr);
```

👉 Use these instead of `*(volatile u32 *)addr` (portable + endian-safe).

---

# 🔹 4. Example: Accessing a Device Register

Suppose device register base = `0x48060000`, size = 0x1000, and we want to write/read from offset `0x10`.

```c
#include <linux/io.h>
#include <linux/module.h>
#include <linux/init.h>

#define DEV_BASE   0x48060000
#define DEV_SIZE   0x1000
#define REG_OFFSET 0x10

static void __iomem *reg_base;

static int __init mydrv_init(void)
{
    u32 val;

    // Map physical → virtual
    reg_base = ioremap(DEV_BASE, DEV_SIZE);
    if (!reg_base) {
        pr_err("ioremap failed\n");
        return -ENOMEM;
    }

    // Write to register
    iowrite32(0xABCD1234, reg_base + REG_OFFSET);

    // Read back
    val = ioread32(reg_base + REG_OFFSET);
    pr_info("Register value: 0x%x\n", val);

    return 0;
}

static void __exit mydrv_exit(void)
{
    iounmap(reg_base);
    pr_info("Driver exit\n");
}

module_init(mydrv_init);
module_exit(mydrv_exit);

MODULE_LICENSE("GPL");
```

---

# 🔹 5. Bulk I/O APIs

For copying multiple values to/from device memory:

```c
void ioread32_rep(void __iomem *addr, u32 *dst, unsigned long count);
void iowrite32_rep(void __iomem *addr, const u32 *src, unsigned long count);
```

---

# 🔹 6. When not to use `ioremap()`?

* **I/O port (x86 legacy I/O space)** → use `inb()/outb()` APIs instead.
* For **DMA buffers** → use `dma_alloc_coherent()` (not `ioremap`).

---

✅ **In summary:**

* Use `ioremap()` to map device physical registers → kernel virtual.
* Access registers using `ioreadX()/iowriteX()` APIs.
* Always `iounmap()` when done (or use `devm_*` helpers).

---

👉 Do you want me to also draw a **block diagram (physical address → ioremap → kernel virtual space → driver)** so it’s easier to visualize?


Here’s the **deep explanation + block diagram** you asked for:

---

# 🔹 `ioremap()` and I/O Read/Write APIs in Linux Kernel

When a device exposes **memory-mapped I/O (MMIO)** registers, the CPU cannot directly dereference those **physical addresses** from kernel space. Instead, the kernel must first **map them into its virtual address space** using `ioremap()`.

---

## 1. Flow of Access

```
+------------------+       +------------------+       +---------------------+
|  Physical Addr   | --->  |   ioremap()      | --->  |  Kernel Virtual Addr |
| (e.g. 0x48000000)|       | (page table map) |       | (used by driver)     |
+------------------+       +------------------+       +---------------------+
                                                             |
                                                             v
                                                +--------------------------+
                                                | ioread*/iowrite* APIs    |
                                                | (safe register access)   |
                                                +--------------------------+
```

---

## 2. Key APIs

### a) Mapping APIs

* `void __iomem *ioremap(phys_addr_t phys_addr, size_t size);`

  * Maps physical I/O memory into **kernel virtual address** space (non-cached).
* `void iounmap(void __iomem *addr);`

  * Unmaps the region when driver is unloaded.

---

### b) I/O Access APIs

> These must be used instead of `*ptr = val` to ensure correct ordering & endianness.

* **Read APIs**

  * `u8  ioread8(void __iomem *addr);`
  * `u16 ioread16(void __iomem *addr);`
  * `u32 ioread32(void __iomem *addr);`
  * `u64 ioread64(void __iomem *addr);`

* **Write APIs**

  * `void iowrite8(u8 val, void __iomem *addr);`
  * `void iowrite16(u16 val, void __iomem *addr);`
  * `void iowrite32(u32 val, void __iomem *addr);`
  * `void iowrite64(u64 val, void __iomem *addr);`

---

### c) Bulk Access APIs (for FIFOs, buffers)

* `void ioread32_rep(void __iomem *addr, void *dst, unsigned long count);`
* `void iowrite32_rep(void __iomem *addr, const void *src, unsigned long count);`

---

## 3. Example Driver Snippet

```c
#include <linux/io.h>

#define DEVICE_BASE_PHYS  0x48000000
#define DEVICE_REG_SIZE   0x1000
#define REG_OFFSET        0x20

static void __iomem *base;

static int mydrv_probe(struct platform_device *pdev)
{
    u32 val;

    // Step 1: Map physical base to virtual
    base = ioremap(DEVICE_BASE_PHYS, DEVICE_REG_SIZE);
    if (!base)
        return -ENOMEM;

    // Step 2: Read register
    val = ioread32(base + REG_OFFSET);
    pr_info("Register value: 0x%x\n", val);

    // Step 3: Write register
    iowrite32(val | 0x01, base + REG_OFFSET);

    return 0;
}

static int mydrv_remove(struct platform_device *pdev)
{
    iounmap(base);
    return 0;
}
```

---

## 4. Why not direct pointer dereference?

* CPU caching may reorder or delay writes.
* MMIO regions must be **non-cached** and **strongly ordered**.
* `ioread*/iowrite*` ensures **barriers + ordering** across different CPU architectures.

---

👉 Do you want me to also expand this with **a real-world example** (say, GPIO register read/write on ARM SoC using `ioremap`) so you can directly try it out on your board?
