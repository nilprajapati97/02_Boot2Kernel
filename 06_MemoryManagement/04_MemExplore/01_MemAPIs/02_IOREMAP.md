Great question 👍. `ioremap()` is one of the most important APIs in Linux kernel device driver development, especially for **memory-mapped I/O (MMIO)**.
Let’s go step by step from **basic concept → usage → internals → types → cleanup**.

---

# 🔹 1. Why do we need `ioremap()`?

* In embedded/Linux systems, **peripheral registers** (UART, I2C, GPIO, etc.) are not accessed with normal CPU instructions like RAM.
* Instead, SoCs map these device registers into the **physical address space** (MMIO).
* User/kernel cannot directly access these *physical* addresses → kernel uses **virtual addresses**.
* `ioremap()` creates a **kernel virtual mapping** to access such physical device addresses.

👉 Without `ioremap()`, if driver directly dereferences a physical address → **page fault / crash**.

---

# 🔹 2. Function Prototype

```c
#include <linux/io.h>

void __iomem *ioremap(phys_addr_t phys_addr, size_t size);
```

* **phys\_addr\_t phys\_addr** → Physical address (start of register region).
* **size\_t size** → Number of bytes to map.
* **Return**: Virtual address (`__iomem` pointer) usable with `readl()`, `writel()`, etc.

> `__iomem` is a special annotation to indicate the pointer refers to **I/O memory**, not normal RAM.

---

# 🔹 3. Example in a Driver

Suppose we have a UART controller mapped at **0x48020000**, size **0x1000**:

```c
#define UART_BASE_PHYS   0x48020000
#define UART_SIZE        0x1000

static void __iomem *uart_base;

static int __init uart_driver_init(void)
{
    // Map physical to virtual
    uart_base = ioremap(UART_BASE_PHYS, UART_SIZE);
    if (!uart_base) {
        pr_err("Failed to ioremap UART registers\n");
        return -ENOMEM;
    }

    // Write to UART register (offset 0x00 → TX register for example)
    writel(0x55, uart_base + 0x00);

    // Read from UART register (offset 0x04 → RX register for example)
    pr_info("UART RX value: %x\n", readl(uart_base + 0x04));

    return 0;
}

static void __exit uart_driver_exit(void)
{
    // Unmap before exit
    iounmap(uart_base);
}

module_init(uart_driver_init);
module_exit(uart_driver_exit);
```

---

# 🔹 4. Variants of `ioremap()`

1. **`ioremap()`**

   * Default mapping (non-cacheable, strongly ordered).

2. **`ioremap_nocache()`** *(old, deprecated)*

   * Same as `ioremap()` today (kept for backward compatibility).
   * Devices must not use caching.

3. **`ioremap_wc()`** (Write Combine)

   * Allows **write combining** (CPU batches writes for performance).
   * Useful for **framebuffer/video memory**.

4. **`ioremap_uc()`** (Uncached)

   * Explicit uncached mapping.

5. **`ioremap_prot(phys_addr, size, prot)`**

   * Low-level function where you define **page protection flags** manually.

---

# 🔹 5. Cleanup

Always call:

```c
iounmap(uart_base);
```

to release mapping, otherwise → **memory leak**.

---

# 🔹 6. Internals (What happens inside `ioremap()`)

* Kernel maintains **page tables**.
* When you call `ioremap()`:

  1. It aligns the physical address to a page boundary.
  2. Reserves that region (prevents conflicts).
  3. Creates a kernel virtual mapping with appropriate **page attributes** (uncached, device memory).
  4. Returns the new virtual address.

So CPU sees a **virtual address**, but MMU routes it to device registers.

---

# 🔹 7. Accessing MMIO Safely

⚠️ Never do:

```c
*(volatile unsigned int *)(uart_base + 0x00) = 0x55;   // BAD
```

Instead use kernel APIs:

```c
writel(0x55, uart_base + 0x00);   // SAFE
value = readl(uart_base + 0x04);  // SAFE
```

Why?

* Kernel’s accessor APIs (`readl()`, `writel()`, `iowrite32()`, etc.) handle **endianness, memory barriers, ordering issues**.
* Direct dereferencing is architecture-dependent and unsafe.

---

# 🔹 8. Typical Flow in Device Driver

1. Parse device base address (from **Device Tree / PCI BAR / platform data**).
2. Use `ioremap()` in `probe()` function.
3. Use `readl()/writel()` to access registers.
4. Free with `iounmap()` in `remove()` function.

---

# 🔹 9. Real Device Tree Binding Example

```dts
uart0: serial@48020000 {
    compatible = "ti,omap-uart";
    reg = <0x48020000 0x1000>;
};
```

In driver:

```c
res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
uart_base = devm_ioremap_resource(&pdev->dev, res);
```

👉 `devm_ioremap_resource()` is preferred because it auto-unmaps on driver detach.

---

✅ **Summary**

* `ioremap()` maps physical MMIO registers into kernel’s virtual space.
* Use with `readl()/writel()` not raw pointers.
* Variants available (`ioremap_wc()`, `ioremap_uc()`).
* Always `iounmap()` or use `devm_ioremap_resource()`.
* Essential for writing Linux device drivers.

---

Do you want me to also **draw a block diagram (physical address → ioremap → kernel virtual space → driver)** so it’s easier to visualize?
Yes 👍 that’s the best way to see what `ioremap()` is actually doing inside the kernel. Here’s a simple **block diagram** representation:

---

```
                 +-------------------+
                 |   Device/SoC HW   |
                 | (Registers, MMIO) |
                 +-------------------+
                         |
                         |  Physical Address (e.g. 0x48020000)
                         v
                 +-------------------+
                 |  Physical Address |
                 |  (Not directly    |
                 |   accessible by   |
                 |   kernel code)    |
                 +-------------------+
                         |
                         |  ioremap()
                         v
   =================================================================
   |                  Kernel Virtual Address Space                  |
   =================================================================
                         |
                 +-------------------+
                 | Virtual Address   |
                 | returned by       |
                 | ioremap()         |
                 +-------------------+
                         |
                         |  Access via
                         |  readl()/writel()
                         v
                 +-------------------+
                 |   Device Driver   |
                 | (probe/init code) |
                 +-------------------+
```

---

🔑 **Flow explanation**

1. Hardware exposes control registers at some **physical address**.
2. Kernel **cannot directly dereference** physical addresses → it must map them.
3. `ioremap()` creates a **kernel virtual mapping** of that region.
4. Driver gets the returned pointer and uses `readl()/writel()` to safely read/write registers.
5. On unload, `iounmap()` removes the mapping.

---

Would you like me to extend this diagram to also show the **Device Tree flow** (DTS → platform driver → `devm_ioremap_resource()` → driver)? That’s the most common path on embedded SoCs.
