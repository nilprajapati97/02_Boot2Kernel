Here's a complete **working Linux character device driver** that implements `mmap()` support along with a simple **user-space test program** to demonstrate memory-mapped access.

---

## ✅ Overview

* Kernel driver allocates one page of memory
* Exposes it to user space via `mmap()`
* User program writes and reads from that memory region

---

## 🧠 Driver Code (kernel module)

### 📁 File: `mmap_driver.c`

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "mmapdemo"
#define MEM_SIZE PAGE_SIZE

static dev_t devno;
static struct cdev mmap_cdev;
static char *kernel_buffer;

static int mmap_open(struct inode *inode, struct file *file) {
    pr_info("mmap: device opened\n");
    return 0;
}

static int mmap_release(struct inode *inode, struct file *file) {
    pr_info("mmap: device closed\n");
    return 0;
}

static int mmap_mmap(struct file *file, struct vm_area_struct *vma) {
    unsigned long pfn = virt_to_phys(kernel_buffer) >> PAGE_SHIFT;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > MEM_SIZE)
        return -EINVAL;

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        pr_err("remap_pfn_range failed\n");
        return -EAGAIN;
    }

    pr_info("mmap: remapped 0x%lx to user at 0x%lx\n", pfn << PAGE_SHIFT, vma->vm_start);
    return 0;
}

static struct file_operations mmap_fops = {
    .owner = THIS_MODULE,
    .open = mmap_open,
    .release = mmap_release,
    .mmap = mmap_mmap,
};

static int __init mmap_init(void) {
    alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    cdev_init(&mmap_cdev, &mmap_fops);
    cdev_add(&mmap_cdev, devno, 1);

    kernel_buffer = (char *)kmalloc(MEM_SIZE, GFP_KERNEL);
    if (!kernel_buffer)
        return -ENOMEM;

    strcpy(kernel_buffer, "Hello from kernel space!");

    pr_info("mmap: driver loaded with major %d\n", MAJOR(devno));
    return 0;
}

static void __exit mmap_exit(void) {
    cdev_del(&mmap_cdev);
    unregister_chrdev_region(devno, 1);
    kfree(kernel_buffer);
    pr_info("mmap: driver unloaded\n");
}

module_init(mmap_init);
module_exit(mmap_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Prajapati");
MODULE_DESCRIPTION("Simple mmap demo driver");
```

---

## 🧪 User-Space Program

### 📁 File: `test_mmap.c`

```c
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define DEVICE "/dev/mmapdemo"
#define SIZE 4096

int main() {
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char *mapped = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("Data read from kernel: %s\n", mapped);

    strcpy(mapped, "Modified by user-space");
    printf("Data written to shared memory.\n");

    munmap(mapped, SIZE);
    close(fd);
    return 0;
}
```

---

## 🔧 Steps to Build and Run

### 1. **Save kernel code** in `mmap_driver.c`

### 2. **Create Makefile**

```Makefile
obj-m += mmap_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

### 3. **Build the module**

```bash
make
```

### 4. **Insert module and create device node**

```bash
sudo insmod mmap_driver.ko
dmesg | tail   # Note the major number

# Replace X with actual major number from dmesg
sudo mknod /dev/mmapdemo c X 0
sudo chmod 666 /dev/mmapdemo
```

### 5. **Compile user program**

```bash
gcc test_mmap.c -o test_mmap
```

### 6. **Run user program**

```bash
./test_mmap
```

Expected output:

```bash
Data read from kernel: Hello from kernel space!
Data written to shared memory.
```

### 7. **Check kernel memory updated**

You can read the `kernel_buffer` content by adding a `proc` entry or use `pr_info` logs in driver to verify.

---

## 🔁 Unload Module

```bash
sudo rmmod mmap_driver
```

---

## ✅ Interview Tips

* Explain how `remap_pfn_range()` maps physical pages to user-space virtual address
* Differentiate `vmalloc` + `vm_insert_page()` vs `kmalloc` + `remap_pfn_range()`
* Understand `vm_area_struct` and user memory layout
* Know page table translation, memory protection, cache control

---

Let me know if you'd like a version that uses `vmalloc()` or supports multiple mappings, DMA-safe memory, or mapping hardware registers.
