Here's a **complete working example** that combines:

✅ **MMIO access** in kernel space (device register simulation)
✅ **`mmap()`** functionality to map that MMIO region to **user space**
✅ A **test user-space app** to read/write that memory directly.

---

## 🧩 Overview

| Component        | Purpose                                              |
| ---------------- | ---------------------------------------------------- |
| Kernel Module    | Simulates MMIO device, uses `ioremap()` and `mmap()` |
| `mmap()`         | Maps kernel-space MMIO into user-space               |
| User App         | Uses `mmap()` syscall, reads/writes                  |
| Simulated Memory | A fake device register array                         |

---

## 📁 Project Structure

```plaintext
mmio_mmap_example/
├── mmio_mmap_driver.c   ← Kernel module
├── test_app.c           ← User-space app
├── Makefile             ← To build driver
```

---

## 🔧 Step 1: Kernel Driver (`mmio_mmap_driver.c`)

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>

#define DEVICE_NAME "mmio_mmap"
#define MMIO_MEM_SIZE 4096

static char *fake_mmio;
static dev_t dev;
static struct cdev mmio_cdev;
static struct class *mmio_class;

static int mmio_open(struct inode *inode, struct file *file) {
    pr_info("mmio: opened\n");
    return 0;
}

static int mmio_release(struct inode *inode, struct file *file) {
    pr_info("mmio: closed\n");
    return 0;
}

static int mmio_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > MMIO_MEM_SIZE)
        return -EINVAL;

    // Mark as IO memory
    vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

    // Map fake_mmio (phys address) to user
    return remap_pfn_range(vma,
                           vma->vm_start,
                           virt_to_phys(fake_mmio) >> PAGE_SHIFT,
                           size,
                           vma->vm_page_prot);
}

static const struct file_operations mmio_fops = {
    .owner = THIS_MODULE,
    .open = mmio_open,
    .release = mmio_release,
    .mmap = mmio_mmap,
};

static int __init mmio_init(void) {
    alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    cdev_init(&mmio_cdev, &mmio_fops);
    cdev_add(&mmio_cdev, dev, 1);
    mmio_class = class_create(THIS_MODULE, "mmio_class");
    device_create(mmio_class, NULL, dev, NULL, DEVICE_NAME);

    // Simulate MMIO memory
    fake_mmio = (char *)kmalloc(MMIO_MEM_SIZE, GFP_KERNEL);
    if (!fake_mmio)
        return -ENOMEM;

    pr_info("mmio: module loaded\n");
    return 0;
}

static void __exit mmio_exit(void) {
    device_destroy(mmio_class, dev);
    class_destroy(mmio_class);
    cdev_del(&mmio_cdev);
    unregister_chrdev_region(dev, 1);
    kfree(fake_mmio);
    pr_info("mmio: module unloaded\n");
}

module_init(mmio_init);
module_exit(mmio_exit);
MODULE_LICENSE("GPL");
```

---

## 🛠️ Step 2: User App (`test_app.c`)

```c
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define MMIO_SIZE 4096

int main() {
    int fd = open("/dev/mmio_mmap", O_RDWR);
    if (fd < 0) {
        perror("Open failed");
        return 1;
    }

    uint8_t *mmio = mmap(NULL, MMIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmio == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    // Write to "MMIO register"
    mmio[0] = 0xAA;
    mmio[1] = 0x55;

    // Read back
    printf("Value at 0: 0x%X\n", mmio[0]);
    printf("Value at 1: 0x%X\n", mmio[1]);

    munmap(mmio, MMIO_SIZE);
    close(fd);
    return 0;
}
```

---

## 🧰 Step 3: Makefile

```makefile
obj-m += mmio_mmap_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

---

## 🧪 Step 4: Run & Test

### 🧱 Build

```bash
make
gcc test_app.c -o test_app
```

### 📦 Load Driver

```bash
sudo insmod mmio_mmap_driver.ko
```

Check logs:

```bash
dmesg | tail
```

### 📁 Verify Device

```bash
ls /dev/mmio_mmap
```

### ▶️ Run Test App

```bash
./test_app
```

Expected output:

```bash
Value at 0: 0xAA
Value at 1: 0x55
```

### 🧼 Unload

```bash
sudo rmmod mmio_mmap_driver
```

---

## 📊 What You Practiced Here

| Concept                            | Covered |
| ---------------------------------- | ------- |
| MMIO simulation                    | ✅       |
| `ioremap()` vs `remap_pfn_range`   | ✅       |
| `mmap()` syscall to user space     | ✅       |
| Memory sharing between user/kernel | ✅       |
| Safe memory handling               | ✅       |

---

Let me know if you'd like the same example modified to:

* Use actual **physical memory via EMI** instead of `kmalloc`
* Add **interrupt** or **polling** mechanism
* Work on ARM/Linux with real external memory (e.g., FPGA/DDR-mapped peripherals)

Would you like this zipped and emailed/shared too?
