# Linux Character Driver Model – Detailed Design

A Linux Character Driver (Char Driver) provides a byte-stream interface between **user space** and a **device**. Unlike block drivers, char drivers do not manage data in fixed-size blocks; they process data sequentially.

Examples:

* UART/Serial Driver (`/dev/ttyS0`)
* I2C EEPROM Driver
* GPIO Character Driver
* Input Devices
* Custom FPGA Drivers

---
# 1. Linux Device Driver Architecture

```text
+----------------------+
|    User Application  |
+----------+-----------+
           |
           | open(), read(), write(), ioctl()
           v
+----------------------+
|      VFS Layer       |
+----------+-----------+
           |
           v
+----------------------+
| Character Driver     |
| file_operations      |
+----------+-----------+
           |
           v
+----------------------+
| Device Controller    |
| Registers / HW       |
+----------------------+
```

Flow:

```text
User Process
     |
read(fd)
     |
     v
sys_read()
     |
     v
VFS
     |
     v
file_operations->read()
     |
     v
Driver
     |
     v
Hardware
```

---

# 2. Major Components

## A. Device Number

Linux identifies devices using:

```c
dev_t dev;
```

Contains:

```text
Major Number -> Driver
Minor Number -> Device Instance
```

Example:

```text
Major = 240
Minor = 0
```

Means:

```text
Driver = My Driver
Device = First Device
```

---

### Device Number Allocation

Static:

```c
register_chrdev_region(dev, count, "mydrv");
```

Dynamic:

```c
alloc_chrdev_region(&dev, 0, 1, "mydrv");
```

Kernel allocates major number automatically.

---

# 3. cdev Structure

Kernel uses `struct cdev`.

```c
struct cdev {
    struct kobject kobj;
    struct module *owner;
    const struct file_operations *ops;
};
```

Initialization:

```c
cdev_init(&my_cdev, &fops);

cdev_add(&my_cdev, dev, 1);
```

---

# 4. Device Creation Flow

```text
Driver Load
    |
alloc_chrdev_region()
    |
cdev_init()
    |
cdev_add()
    |
class_create()
    |
device_create()
    |
/dev/mychar
```

Example:

```c
class_create();
device_create();
```

Creates:

```bash
/dev/mychar
```

---

# 5. file_operations Structure

Most important structure.

```c
struct file_operations {
    int (*open)(struct inode *, struct file *);
    int (*release)(struct inode *, struct file *);
    ssize_t (*read)(struct file *,
                    char __user *,
                    size_t,
                    loff_t *);

    ssize_t (*write)(struct file *,
                     const char __user *,
                     size_t,
                     loff_t *);

    long (*unlocked_ioctl)(
                     struct file *,
                     unsigned int,
                     unsigned long);

    int (*mmap)(struct file *,
                struct vm_area_struct *);
};
```

---

# 6. Open System Call Internals

User:

```c
fd = open("/dev/mychar", O_RDWR);
```

Kernel Path:

```text
glibc
  |
sys_open()
  |
do_sys_open()
  |
path_lookup()
  |
VFS
  |
inode
  |
cdev
  |
fops->open()
```

Driver:

```c
static int drv_open(struct inode *inode,
                    struct file *file)
{
    return 0;
}
```

---

# 7. Read System Call Internals

User:

```c
read(fd, buf, 100);
```

Flow:

```text
sys_read()
    |
ksys_read()
    |
vfs_read()
    |
file->f_op->read()
    |
driver_read()
```

Driver:

```c
ssize_t drv_read(struct file *file,
                 char __user *buf,
                 size_t count,
                 loff_t *ppos)
{
}
```

---

## Data Transfer

Kernel cannot directly access user buffer.

Wrong:

```c
buf[0] = data;
```

Correct:

```c
copy_to_user(buf,
             kernel_buffer,
             count);
```

Internally:

```text
Kernel Space
     |
copy_to_user()
     |
MMU Validation
     |
Page Fault Handling
     |
User Space
```

---

# 8. Write System Call Internals

User:

```c
write(fd, buf, len);
```

Flow:

```text
sys_write()
    |
vfs_write()
    |
driver_write()
```

Driver:

```c
ssize_t drv_write(struct file *file,
                  const char __user *buf,
                  size_t count,
                  loff_t *ppos)
{
}
```

Receive user data:

```c
copy_from_user(kernel_buf,
               buf,
               count);
```

---

# 9. ioctl Internals

Used for control operations.

User:

```c
ioctl(fd, CMD_SET_SPEED, 115200);
```

Kernel:

```text
sys_ioctl()
     |
vfs_ioctl()
     |
unlocked_ioctl()
```

Driver:

```c
long drv_ioctl(struct file *file,
               unsigned int cmd,
               unsigned long arg)
{
}
```

---

# 10. Poll Mechanism

Used by:

```c
select()
poll()
epoll()
```

Driver:

```c
unsigned int drv_poll(struct file *file,
                      poll_table *wait)
{
}
```

Flow:

```text
Application
     |
poll()
     |
driver_poll()
     |
wait queue
     |
event occurs
     |
wake_up()
```

---

# 11. Blocking Read Design

Without Data:

```text
User read()
      |
Driver
      |
sleep
      |
wait_event()
```

Code:

```c
wait_event_interruptible(
    my_queue,
    data_available);
```

Wakeup:

```c
wake_up_interruptible(&my_queue);
```

---

# 12. Wait Queue Internals

Structure:

```c
wait_queue_head_t queue;
```

Initialization:

```c
init_waitqueue_head(&queue);
```

Internally:

```text
wait_queue_head
      |
      +-- task1
      +-- task2
      +-- task3
```

Scheduler blocks tasks.

---

# 13. Interrupt Driven Char Driver

Architecture:

```text
Hardware
    |
IRQ
    |
ISR
    |
Buffer Data
    |
wake_up()
    |
User Read()
```

ISR:

```c
irqreturn_t drv_isr(int irq,
                    void *dev)
{
    wake_up_interruptible(&queue);

    return IRQ_HANDLED;
}
```

---

# 14. Driver Private Data

Every device instance has private data.

```c
struct my_device {
    struct cdev cdev;

    char buffer[1024];

    spinlock_t lock;

    wait_queue_head_t queue;
};
```

Open:

```c
file->private_data = mydev;
```

Access:

```c
struct my_device *dev;

dev = file->private_data;
```

---

# 15. Concurrency Design

Multiple processes may access:

```text
Process A
Process B
Process C
```

Shared Buffer:

```text
Buffer
```

Need protection.

---

### Mutex

```c
struct mutex lock;

mutex_lock(&lock);

critical section

mutex_unlock(&lock);
```

Used in:

```text
Process Context
```

Can sleep.

---

### Spinlock

```c
spin_lock_irqsave();

critical section

spin_unlock_irqrestore();
```

Used in:

```text
Interrupt Context
```

Cannot sleep.

---

# 16. Memory Allocation

Kernel allocation:

```c
kmalloc()
kzalloc()
vmalloc()
```

Example:

```c
buf = kzalloc(1024,
              GFP_KERNEL);
```

Internally:

```text
Buddy Allocator
      |
SLAB/SLUB
      |
kmalloc()
```

---

# 17. mmap Support

User:

```c
mmap()
```

Driver:

```c
static int drv_mmap(struct file *filp,
                    struct vm_area_struct *vma)
{
}
```

Flow:

```text
Device Memory
       |
remap_pfn_range()
       |
User Virtual Address
```

Avoids:

```text
copy_to_user()
copy_from_user()
```

Zero-copy transfer.

---

# 18. Complete Driver Initialization Flow

```text
module_init()
      |
alloc_chrdev_region()
      |
cdev_init()
      |
cdev_add()
      |
class_create()
      |
device_create()
      |
request_irq()
      |
Driver Ready
```

---

# 19. Driver Removal Flow

```text
module_exit()
      |
free_irq()
      |
device_destroy()
      |
class_destroy()
      |
cdev_del()
      |
unregister_chrdev_region()
```

---

# 20. Interview Questions (AMD / NVIDIA / Qualcomm)

### Q1: Difference between cdev_add() and register_chrdev()?

**Answer:**

`register_chrdev()` is the old API.

Modern drivers use:

```c
alloc_chrdev_region()
cdev_init()
cdev_add()
```

because it supports multiple devices and integrates with the driver model.

---

### Q2: Why use copy_to_user()?

**Answer:**

User-space memory is protected by the MMU.

Direct access may cause:

```text
Kernel Oops
Page Fault
Security Issues
```

Hence:

```c
copy_to_user()
copy_from_user()
```

---

### Q3: Why file->private_data?

**Answer:**

Stores per-open device context.

Example:

```c
file->private_data = dev;
```

Allows all callbacks to access device data.

---

### Q4: Can ISR call mutex_lock()?

**Answer:**

No.

ISR runs in atomic context.

Mutex may sleep.

Use:

```c
spin_lock_irqsave()
```

instead.

---

### Q5: Why wait queue instead of polling?

**Answer:**

Polling wastes CPU cycles.

Wait queue:

```text
Sleep
Wake on Event
```

Lower CPU utilization and latency.

---

### Q6: How does VFS find your read() callback?

**Answer:**

```text
inode
  |
cdev
  |
file_operations
  |
read pointer
```

When `cdev_add()` is done, VFS associates the inode with the driver's `file_operations`. Thus `read()` eventually invokes `fops->read()`.

---

For AMD, NVIDIA, and Qualcomm kernel interviews, a common follow-up is: **"Trace `read()` from userspace to hardware and back."** You should be able to explain the complete chain:

```text
read()
 → glibc
 → syscall
 → VFS
 → file_operations->read()
 → Driver Buffer
 → ISR/DMA fills buffer
 → copy_to_user()
 → userspace
```

and discuss synchronization (mutex/spinlock), wait queues, interrupts, DMA, and memory barriers used in the data path.

Below is a **simple Linux Character Driver** that supports:

* `open()`
* `read()`
* `write()`
* `release()`
* Dynamic major number allocation
* `/dev/mychardev` creation

---

# Character Driver Code

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "mychardev"
#define BUF_SIZE    100

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;

static char kernel_buffer[BUF_SIZE] = "Hello from Kernel Driver\n";

/* Open */
static int my_open(struct inode *inode, struct file *file)
{
    pr_info("Device Opened\n");
    return 0;
}

/* Release */
static int my_release(struct inode *inode, struct file *file)
{
    pr_info("Device Closed\n");
    return 0;
}

/* Read */
static ssize_t my_read(struct file *file,
                       char __user *buf,
                       size_t len,
                       loff_t *offset)
{
    int bytes_to_read;

    if (*offset >= strlen(kernel_buffer))
        return 0; /* EOF */

    bytes_to_read = min((size_t)(strlen(kernel_buffer) - *offset), len);

    if (copy_to_user(buf,
                     kernel_buffer + *offset,
                     bytes_to_read))
        return -EFAULT;

    *offset += bytes_to_read;

    pr_info("Read %d bytes\n", bytes_to_read);

    return bytes_to_read;
}

/* Write */
static ssize_t my_write(struct file *file,
                        const char __user *buf,
                        size_t len,
                        loff_t *offset)
{
    size_t bytes_to_write;

    bytes_to_write = min(len, (size_t)(BUF_SIZE - 1));

    if (copy_from_user(kernel_buffer,
                       buf,
                       bytes_to_write))
        return -EFAULT;

    kernel_buffer[bytes_to_write] = '\0';

    pr_info("Received from user: %s\n", kernel_buffer);

    return bytes_to_write;
}

/* File operations */
static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .read    = my_read,
    .write   = my_write,
    .release = my_release,
};

/* Module Init */
static int __init my_driver_init(void)
{
    int ret;

    /* Allocate Major/Minor */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0)
        return ret;

    pr_info("Major=%d Minor=%d\n",
            MAJOR(dev_num),
            MINOR(dev_num));

    /* Initialize cdev */
    cdev_init(&my_cdev, &fops);

    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret)
        goto unregister_dev;

    /* Create class */
    my_class = class_create(DEVICE_NAME);
    if (IS_ERR(my_class)) {
        ret = PTR_ERR(my_class);
        goto del_cdev;
    }

    /* Create /dev node */
    device_create(my_class,
                  NULL,
                  dev_num,
                  NULL,
                  DEVICE_NAME);

    pr_info("Character Driver Loaded\n");

    return 0;

del_cdev:
    cdev_del(&my_cdev);

unregister_dev:
    unregister_chrdev_region(dev_num, 1);

    return ret;
}

/* Module Exit */
static void __exit my_driver_exit(void)
{
    device_destroy(my_class, dev_num);
    class_destroy(my_class);

    cdev_del(&my_cdev);

    unregister_chrdev_region(dev_num, 1);

    pr_info("Character Driver Unloaded\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil");
MODULE_DESCRIPTION("Simple Character Driver");
```

---

# Makefile

```makefile
obj-m += mychardrv.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
```

---

# Build Driver

```bash
make
```

Output:

```text
mychardrv.ko
```

---

# Insert Module

```bash
sudo insmod mychardrv.ko
```

Check logs:

```bash
dmesg | tail
```

Example:

```text
Major=240 Minor=0
Character Driver Loaded
```

---

# Verify Device Node

```bash
ls -l /dev/mychardev
```

---

# Read from Driver

```bash
cat /dev/mychardev
```

Output:

```text
Hello from Kernel Driver
```

Kernel log:

```text
Read 25 bytes
```

---

# Write to Driver

```bash
echo "Linux Driver" > /dev/mychardev
```

Kernel log:

```text
Received from user: Linux Driver
```

---

# Remove Module

```bash
sudo rmmod mychardrv
```

Check:

```bash
dmesg | tail
```

Output:

```text
Character Driver Unloaded
```

---

## Internal Flow

```text
insmod
  |
module_init()
  |
alloc_chrdev_region()
  |
cdev_add()
  |
device_create()
  |
/dev/mychardev
```

```text
cat /dev/mychardev
    |
sys_read()
    |
vfs_read()
    |
my_read()
    |
copy_to_user()
```

```text
echo "abc" > /dev/mychardev
      |
sys_write()
      |
vfs_write()
      |
my_write()
      |
copy_from_user()
```

This is the most commonly asked **basic character driver** in Linux kernel interviews before moving to wait queues, poll, interrupts, platform drivers, and device-tree-based drivers.

Below is a **simple Linux Character Driver** that supports:

* `open()`
* `read()`
* `write()`
* `release()`
* Dynamic major number allocation
* `/dev/mychardev` creation

---

# Character Driver Code

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "mychardev"
#define BUF_SIZE    100

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *my_class;

static char kernel_buffer[BUF_SIZE] = "Hello from Kernel Driver\n";

/* Open */
static int my_open(struct inode *inode, struct file *file)
{
    pr_info("Device Opened\n");
    return 0;
}

/* Release */
static int my_release(struct inode *inode, struct file *file)
{
    pr_info("Device Closed\n");
    return 0;
}

/* Read */
static ssize_t my_read(struct file *file,
                       char __user *buf,
                       size_t len,
                       loff_t *offset)
{
    int bytes_to_read;

    if (*offset >= strlen(kernel_buffer))
        return 0; /* EOF */

    bytes_to_read = min((size_t)(strlen(kernel_buffer) - *offset), len);

    if (copy_to_user(buf,
                     kernel_buffer + *offset,
                     bytes_to_read))
        return -EFAULT;

    *offset += bytes_to_read;

    pr_info("Read %d bytes\n", bytes_to_read);

    return bytes_to_read;
}

/* Write */
static ssize_t my_write(struct file *file,
                        const char __user *buf,
                        size_t len,
                        loff_t *offset)
{
    size_t bytes_to_write;

    bytes_to_write = min(len, (size_t)(BUF_SIZE - 1));

    if (copy_from_user(kernel_buffer,
                       buf,
                       bytes_to_write))
        return -EFAULT;

    kernel_buffer[bytes_to_write] = '\0';

    pr_info("Received from user: %s\n", kernel_buffer);

    return bytes_to_write;
}

/* File operations */
static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .read    = my_read,
    .write   = my_write,
    .release = my_release,
};

/* Module Init */
static int __init my_driver_init(void)
{
    int ret;

    /* Allocate Major/Minor */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0)
        return ret;

    pr_info("Major=%d Minor=%d\n",
            MAJOR(dev_num),
            MINOR(dev_num));

    /* Initialize cdev */
    cdev_init(&my_cdev, &fops);

    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret)
        goto unregister_dev;

    /* Create class */
    my_class = class_create(DEVICE_NAME);
    if (IS_ERR(my_class)) {
        ret = PTR_ERR(my_class);
        goto del_cdev;
    }

    /* Create /dev node */
    device_create(my_class,
                  NULL,
                  dev_num,
                  NULL,
                  DEVICE_NAME);

    pr_info("Character Driver Loaded\n");

    return 0;

del_cdev:
    cdev_del(&my_cdev);

unregister_dev:
    unregister_chrdev_region(dev_num, 1);

    return ret;
}

/* Module Exit */
static void __exit my_driver_exit(void)
{
    device_destroy(my_class, dev_num);
    class_destroy(my_class);

    cdev_del(&my_cdev);

    unregister_chrdev_region(dev_num, 1);

    pr_info("Character Driver Unloaded\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil");
MODULE_DESCRIPTION("Simple Character Driver");
```

---

# Makefile

```makefile
obj-m += mychardrv.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
```

---

# Build Driver

```bash
make
```

Output:

```text
mychardrv.ko
```

---

# Insert Module

```bash
sudo insmod mychardrv.ko
```

Check logs:

```bash
dmesg | tail
```

Example:

```text
Major=240 Minor=0
Character Driver Loaded
```

---

# Verify Device Node

```bash
ls -l /dev/mychardev
```

---

# Read from Driver

```bash
cat /dev/mychardev
```

Output:

```text
Hello from Kernel Driver
```

Kernel log:

```text
Read 25 bytes
```

---

# Write to Driver

```bash
echo "Linux Driver" > /dev/mychardev
```

Kernel log:

```text
Received from user: Linux Driver
```

---

# Remove Module

```bash
sudo rmmod mychardrv
```

Check:

```bash
dmesg | tail
```

Output:

```text
Character Driver Unloaded
```

---

## Internal Flow

```text
insmod
  |
module_init()
  |
alloc_chrdev_region()
  |
cdev_add()
  |
device_create()
  |
/dev/mychardev
```

```text
cat /dev/mychardev
    |
sys_read()
    |
vfs_read()
    |
my_read()
    |
copy_to_user()
```

```text
echo "abc" > /dev/mychardev
      |
sys_write()
      |
vfs_write()
      |
my_write()
      |
copy_from_user()
```

This is the most commonly asked **basic character driver** in Linux kernel interviews before moving to wait queues, poll, interrupts, platform drivers, and device-tree-based drivers.
