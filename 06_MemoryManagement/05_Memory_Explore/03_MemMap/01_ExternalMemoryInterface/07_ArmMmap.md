Here is a **complete working MMIO + `mmap()` example** tailored for **ARM/Linux with real external memory**, like an **FPGA-mapped device or custom memory via EMI interface**. This includes:

---

### ✅ Scenario

You have a **memory-mapped FPGA register block** (e.g., `0x40000000`) and want to:

1. Map that memory into kernel via MMIO.
2. Map it into **user-space via `mmap()`** for direct user-space access.
3. Interact using a test app.

---

### ✅ Setup

* ARM SoC (e.g., i.MX, AM335x, Zynq, etc.)
* FPGA/device mapped via EMI to fixed physical address (e.g., `0x40000000`)
* Address is declared in **Device Tree**

---

## 🧩 1. Device Tree Node

```dts
fpga_regs: fpga@40000000 {
    compatible = "mycompany,fpga-mem";
    reg = <0x40000000 0x1000>; // 4KB mapped region
};
```

---

## 🧩 2. Kernel Driver: `fpga_mmap_driver.c`

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/cdev.h>

#define DEVICE_NAME "fpga_mmap"
#define CLASS_NAME  "fpga"

static struct class *fpga_class;
static struct cdev fpga_cdev;
static dev_t dev_no;

static void __iomem *fpga_virt_addr;
static phys_addr_t fpga_phys_addr;
static size_t fpga_size;

/* File ops mmap */
static int fpga_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > fpga_size)
        return -EINVAL;

    return remap_pfn_range(
        vma,
        vma->vm_start,
        fpga_phys_addr >> PAGE_SHIFT,
        size,
        vma->vm_page_prot
    );
}

static int fpga_open(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open  = fpga_open,
    .mmap  = fpga_mmap,
};

static int fpga_probe(struct platform_device *pdev)
{
    struct resource *res;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res)
        return -ENODEV;

    fpga_phys_addr = res->start;
    fpga_size = resource_size(res);

    fpga_virt_addr = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(fpga_virt_addr))
        return PTR_ERR(fpga_virt_addr);

    /* Char device registration */
    alloc_chrdev_region(&dev_no, 0, 1, DEVICE_NAME);
    cdev_init(&fpga_cdev, &fops);
    cdev_add(&fpga_cdev, dev_no, 1);

    fpga_class = class_create(THIS_MODULE, CLASS_NAME);
    device_create(fpga_class, NULL, dev_no, NULL, DEVICE_NAME);

    dev_info(&pdev->dev, "FPGA MMIO driver loaded\n");
    return 0;
}

static int fpga_remove(struct platform_device *pdev)
{
    device_destroy(fpga_class, dev_no);
    class_destroy(fpga_class);
    cdev_del(&fpga_cdev);
    unregister_chrdev_region(dev_no, 1);
    return 0;
}

static const struct of_device_id fpga_of_ids[] = {
    { .compatible = "mycompany,fpga-mem" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fpga_of_ids);

static struct platform_driver fpga_driver = {
    .driver = {
        .name = DEVICE_NAME,
        .of_match_table = fpga_of_ids,
    },
    .probe = fpga_probe,
    .remove = fpga_remove,
};

module_platform_driver(fpga_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("FPGA MMIO + mmap driver");
```

---

## 🧩 3. Test App: `test_fpga_mmap.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_SIZE 0x1000

int main()
{
    int fd = open("/dev/fpga_mmap", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Open failed");
        return 1;
    }

    void *mapped = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return 1;
    }

    volatile unsigned int *reg = (unsigned int *)mapped;

    // Example: write and read FPGA register
    reg[0] = 0xABCD1234;
    printf("Register[0] = 0x%08X\n", reg[0]);

    munmap(mapped, MAP_SIZE);
    close(fd);

    return 0;
}
```

---

## 🔷 Full Flow Summary

```plaintext
+--------------------------+
|  User App (test_fpga)    |
| - mmap()                 |
| - reg[0] = val           |
+------------+-------------+
             |
             v
+------------+-------------+
|  /dev/fpga_mmap          |
| -> mmap() file op        |
| -> remap_pfn_range()     |
+------------+-------------+
             |
             v
+------------+-------------+
| Kernel MMU / Page Table |
| -> Maps to EMI/fixed    |
|    physical region      |
+------------+-------------+
             |
             v
+------------+-------------+
|   External Memory       |
| (FPGA registers at 0x40000000) |
+--------------------------+
```

---

## 🔧 Build and Deploy

### 1. Build driver (simple Makefile)

```makefile
obj-m += fpga_mmap_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

### 2. Load driver

```bash
sudo insmod fpga_mmap_driver.ko
dmesg | tail
```

### 3. Run test

```bash
gcc -o test_fpga test_fpga_mmap.c
sudo ./test_fpga
```

---

## 💡 Interview Tips

* Be ready to explain `ioremap()`, `remap_pfn_range()`, and why `mmap()` is safe for performance.
* Know how EMI or EIM interface is memory-mapped.
* Demonstrate your understanding of cache coherency (MMIO should be non-cacheable).
* Point out that this avoids system calls (faster direct access).

---

Would you like this packaged into a `.zip` file or step-by-step deployment script?
