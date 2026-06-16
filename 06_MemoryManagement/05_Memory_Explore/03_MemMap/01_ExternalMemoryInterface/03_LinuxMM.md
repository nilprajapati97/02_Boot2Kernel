Here’s a **complete, real-world Linux MMIO device driver template**, explained **step by step from scratch**, suitable for interviews and real embedded system work.

This example assumes:

* A hardware device is memory-mapped at **0x10000000**
* Device has registers at offsets:

  * `REG_STATUS` (0x00)
  * `REG_CONTROL` (0x04)
* You want to read/write these safely via an MMIO driver

---

## ✅ GOAL

We will:

1. Define MMIO region via **Device Tree**
2. Write a **platform driver**
3. Use `ioremap()` and `ioread32()/iowrite32()`
4. Expose `read()` and `write()` methods to user space

---

## 📁 1. Device Tree Node (DTS)

Add this to your board's device tree file:

```dts
mydevice@10000000 {
    compatible = "myvendor,mydevice";
    reg = <0x10000000 0x1000>; // 4KB MMIO region
};
```

---

## 📂 2. Driver Code: `mmio_driver.c`

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>

#define DEVICE_NAME "mmio_dev"

#define REG_STATUS_OFFSET   0x00
#define REG_CONTROL_OFFSET  0x04

static void __iomem *mmio_base;
static struct cdev mmio_cdev;
static dev_t dev_num;

// User-space interaction: write control, read status
static ssize_t mmio_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
    u32 val = ioread32(mmio_base + REG_STATUS_OFFSET);
    return copy_to_user(buf, &val, sizeof(val)) ? -EFAULT : sizeof(val);
}

static ssize_t mmio_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
    u32 val;
    if (copy_from_user(&val, buf, sizeof(val)))
        return -EFAULT;

    iowrite32(val, mmio_base + REG_CONTROL_OFFSET);
    return sizeof(val);
}

static struct file_operations mmio_fops = {
    .owner = THIS_MODULE,
    .read  = mmio_read,
    .write = mmio_write,
};

// Driver probe
static int mmio_probe(struct platform_device *pdev) {
    struct resource *res;

    // Get MMIO address from DT
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res)
        return -EINVAL;

    // Map to kernel virtual address
    mmio_base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(mmio_base))
        return PTR_ERR(mmio_base);

    // Create char device
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    cdev_init(&mmio_cdev, &mmio_fops);
    cdev_add(&mmio_cdev, dev_num, 1);

    pr_info("MMIO driver loaded. Mapped to 0x%p\n", mmio_base);
    return 0;
}

static int mmio_remove(struct platform_device *pdev) {
    cdev_del(&mmio_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("MMIO driver removed\n");
    return 0;
}

// Device Match Table
static const struct of_device_id mmio_of_match[] = {
    { .compatible = "myvendor,mydevice", },
    { },
};
MODULE_DEVICE_TABLE(of, mmio_of_match);

// Platform driver struct
static struct platform_driver mmio_platform_driver = {
    .probe  = mmio_probe,
    .remove = mmio_remove,
    .driver = {
        .name           = DEVICE_NAME,
        .of_match_table = mmio_of_match,
    },
};

module_platform_driver(mmio_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Prajapati");
MODULE_DESCRIPTION("Minimal MMIO Device Driver Template");
```

---

## 🛠 3. User Space Test App

### `test_mmio.c`

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/mmio_dev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Write control value
    unsigned int control_val = 0xA5A5;
    write(fd, &control_val, sizeof(control_val));

    // Read status value
    unsigned int status_val;
    read(fd, &status_val, sizeof(status_val));

    printf("Status register value = 0x%X\n", status_val);

    close(fd);
    return 0;
}
```

---

## 🧱 4. Steps to Build and Test

### 🔹 Step 1: Save driver as `mmio_driver.c`

### 🔹 Step 2: Create a simple Makefile

```makefile
obj-m += mmio_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

### 🔹 Step 3: Build and Load Module

```bash
make
sudo insmod mmio_driver.ko
dmesg | tail
```

### 🔹 Step 4: Create Device Node

```bash
# Replace X with major number from dmesg
sudo mknod /dev/mmio_dev c X 0
sudo chmod 666 /dev/mmio_dev
```

### 🔹 Step 5: Run User Program

```bash
gcc test_mmio.c -o test_mmio
./test_mmio
```

---

## 🔍 Deep Dive: Interview-Level Concepts

| Concept                   | Purpose                                          |
| ------------------------- | ------------------------------------------------ |
| `ioremap()`               | Converts physical MMIO address to kernel virtual |
| `__iomem`                 | Compiler annotation for MMIO pointers            |
| `ioread32()`              | Ensures memory barriers + correct access         |
| `platform_get_resource()` | Gets address from device tree                    |
| `devm_ioremap_resource()` | Automatically handles unmap during remove        |
| `cdev_add()`              | Registers character device for user-space I/O    |

---

## 📊 Memory Mapping Summary

| Layer      | API Used               | Notes                            |
| ---------- | ---------------------- | -------------------------------- |
| Kernel     | `ioremap()`            | Use `ioread32` to access         |
| User Space | `read()`/`write()`     | Via `/dev/mmio_dev`              |
| Hardware   | Mapped at `0x10000000` | Setup via bootloader/device tree |

---

## ✅ Conclusion

This MMIO template can be reused for:

* GPIO Controllers
* FPGA Register Access
* Peripheral Control
* FPGA-to-CPU Communication (via memory registers)

---

Would you like a version with `mmap()` support so the user-space can directly map and access MMIO registers using pointers?
