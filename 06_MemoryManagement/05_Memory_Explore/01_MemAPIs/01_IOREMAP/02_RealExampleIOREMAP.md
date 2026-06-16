Yes 👍 that would be the most useful — because just knowing that *“ioremap gives you a kernel virtual pointer”* is theory, but in practice you’ll often need to map a **real SoC peripheral register** (GPIO, UART, I2C, etc.) into your driver and actually **read/write bits**.

Here’s how we can expand with a **real-world ARM SoC GPIO example**:

---

### 1. **Background**

* On an ARM SoC, every peripheral (GPIO, UART, I2C controller…) is memory-mapped.
* Example: GPIO base physical address might be `0x3F200000` (like on Raspberry Pi).
* Registers are offset from that base, e.g.:

  * `GPFSEL1` (function select register) → offset `0x04`
  * `GPSET0` (set pin high) → offset `0x1C`
  * `GPCLR0` (set pin low) → offset `0x28`

We cannot directly dereference physical addresses inside the kernel — **we must use `ioremap()`** to create a kernel virtual mapping.

---

### 2. **Driver Code Example**

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>       // ioremap, readl, writel
#include <linux/init.h>

#define GPIO_BASE_PHYS   0x3F200000   // Example GPIO controller base (SoC-specific)
#define GPIO_SIZE        0x100        // Map 256 bytes (enough for all GPIO regs)

// GPIO register offsets
#define GPFSEL1          0x04
#define GPSET0           0x1C
#define GPCLR0           0x28

static void __iomem *gpio_base;   // Virtual address after ioremap

static int __init gpio_example_init(void)
{
    u32 val;

    pr_info("GPIO example: init\n");

    // Map physical GPIO region into kernel virtual space
    gpio_base = ioremap(GPIO_BASE_PHYS, GPIO_SIZE);
    if (!gpio_base) {
        pr_err("Failed to ioremap GPIO\n");
        return -ENOMEM;
    }

    // Configure GPIO pin 18 as output
    val = readl(gpio_base + GPFSEL1);
    val &= ~(7 << 24);        // clear bits for GPIO18
    val |=  (1 << 24);        // set as output (001)
    writel(val, gpio_base + GPFSEL1);

    // Set GPIO18 high
    writel(1 << 18, gpio_base + GPSET0);

    pr_info("GPIO18 configured as output and set HIGH\n");
    return 0;
}

static void __exit gpio_example_exit(void)
{
    // Set GPIO18 low before exit
    writel(1 << 18, gpio_base + GPCLR0);

    // Unmap memory
    if (gpio_base)
        iounmap(gpio_base);

    pr_info("GPIO example: exit, GPIO18 set LOW\n");
}

module_init(gpio_example_init);
module_exit(gpio_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Example: GPIO register access with ioremap");
```

---

### 3. **Flow / Block Diagram**

```
[Physical GPIO Base Addr: 0x3F200000]
              │
              ▼
        ioremap(phys, size)
              │
              ▼
[Kernel Virtual Addr (gpio_base) in driver]
              │
   +----------+-----------+
   | readl(gpio_base+ofs) |
   | writel(val, gpio_base+ofs) |
   +----------------------+
              │
              ▼
    Actual Register Access on Hardware
```

---

### 4. **Key APIs Used**

* `ioremap(phys_addr, size)` → returns `void __iomem *` (kernel virtual base).
* `iounmap(base)` → unmap when driver exits.
* `readl(addr)` → read 32-bit value from mapped register.
* `writel(val, addr)` → write 32-bit value to mapped register.
* (`readb`/`writeb`, `readw`/`writew` also available for 8/16-bit registers).

---

✅ This is the exact way **Linux drivers control hardware registers** in real boards.
You can take this skeleton, adjust the **base address + offsets from your SoC datasheet**, and it will directly work.

---

Do you want me to extend this with a **step-by-step check** (how to find GPIO base address from DTS or SoC manual, confirm in `/proc/iomem`, then test the driver)?



Yes 👍 — extending with a **real-world, step-by-step check** will make it super practical. Here’s how I’d build it for GPIO registers (common on ARM SoCs):

---

## 🔹 Step-by-Step: GPIO Register Access via `ioremap()`

### 1. **Find GPIO Base Address**

* **Option A: SoC TRM (Technical Reference Manual)**

  * Example: GPIO1 base = `0x4804C000` (on TI AM335x).
* **Option B: From Linux DTS**

  ```dts
  gpio1: gpio@4804c000 {
      compatible = "ti,omap4-gpio";
      reg = <0x4804c000 0x1000>;
      ...
  };
  ```

  * `reg` property → `base = 0x4804C000`, `size = 0x1000`.

---

### 2. **Confirm in `/proc/iomem`**

```sh
cat /proc/iomem | grep gpio
4804c000-4804cfff : 4804c000.gpio
```

Now you know kernel reserved this region for GPIO.

---

### 3. **Map Registers in Driver**

```c
#define GPIO_BASE   0x4804C000
#define GPIO_SIZE   0x1000

void __iomem *gpio_base;

gpio_base = ioremap(GPIO_BASE, GPIO_SIZE);
if (!gpio_base) {
    pr_err("Failed to ioremap GPIO\n");
    return -ENOMEM;
}
```

---

### 4. **Read/Write with Kernel APIs**

Suppose `GPIO_OE` (Output Enable Register) is at offset `0x134`:

```c
#define GPIO_OE        0x134
#define GPIO_DATAOUT   0x13C

// Read OE register
u32 val = ioread32(gpio_base + GPIO_OE);
pr_info("GPIO_OE = 0x%x\n", val);

// Set GPIO as output (clear bit)
val &= ~(1 << 4);
iowrite32(val, gpio_base + GPIO_OE);

// Drive GPIO high
iowrite32(1 << 4, gpio_base + GPIO_DATAOUT);
```

---

### 5. **Clean Up**

```c
iounmap(gpio_base);
```

---

### 6. **Block Diagram**

```
[ SoC Datasheet/Manual ] → [ DTS "reg" property ] → [ /proc/iomem ]
             ↓
     Physical Address (0x4804C000)
             ↓  ioremap()
     Kernel Virtual Address (ex: 0xfedc0000)
             ↓  ioread32()/iowrite32()
         Driver Code Access
```

---

👉 This is exactly how drivers like GPIO, UART, SPI, I2C controllers access hardware registers.

Do you want me to also add a **mini loadable kernel module (LKM)** that does this GPIO toggle so you can directly insmod it on your board?
