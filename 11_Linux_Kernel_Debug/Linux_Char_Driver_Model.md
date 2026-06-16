# Linux Character Driver Model — In-Depth Explanation from Scratch

This guide explains the Linux Character Driver Model from beginner to advanced Linux kernel engineer level, including architecture, kernel internals, driver registration, VFS interaction, memory flow, and interview questions.

---

# 1. What is a Character Driver?

A Character Driver is a Linux kernel driver that transfers data as a stream of bytes between user space and hardware.

Examples:

* UART Driver
* I2C EEPROM Driver
* GPIO Driver
* RTC Driver
* Watchdog Driver
* Sensor Drivers

Data is accessed sequentially like a file.

Example:

```bash
echo "hello" > /dev/mydevice
cat /dev/mydevice
```

---

# 2. Linux Driver Types

Linux supports three major driver types:

| Driver Type      | Example        | Access Method |
| ---------------- | -------------- | ------------- |
| Character Driver | UART, GPIO     | Byte Stream   |
| Block Driver     | eMMC, SSD      | Block Access  |
| Network Driver   | Ethernet, WiFi | Packet Access |

---

# 3. Linux Device Model Architecture

```text
+----------------------+
| User Application     |
+----------+-----------+
           |
           |
           v
+----------------------+
| libc / syscalls      |
+----------+-----------+
           |
           v
+----------------------+
| VFS Layer            |
+----------+-----------+
           |
           v
+----------------------+
| Character Driver     |
+----------+-----------+
           |
           v
+----------------------+
| Hardware             |
+----------------------+
```

Example:

```c
fd = open("/dev/mychar", O_RDWR);
write(fd, buf, len);
read(fd, buf, len);
close(fd);
```

---

# 4. Why Character Driver Exists?

Without a driver:

```text
Application
      |
      X
Hardware Registers
```

Applications cannot directly access hardware.

Reasons:

* Security
* Memory Protection
* Virtual Memory
* Multi-process synchronization

Kernel driver acts as a bridge.

```text
Application
      |
      v
Character Driver
      |
      v
Hardware
```

---

# 5. Linux Device File Concept

Everything in Linux is a file.

Example:

```bash
ls -l /dev
```

Output:

```text
crw-rw-rw- 1 root root 240,0 mychar
```

Meaning:

```text
c = Character Device
240 = Major Number
0 = Minor Number
```

---

# 6. Major and Minor Numbers

Kernel identifies devices using:

```text
dev_t
```

Structure:

```text
31                    20 19                 0
+----------------------+--------------------+
| Major Number         | Minor Number       |
+----------------------+--------------------+
```

Example:

```c
dev_t dev;
alloc_chrdev_region(&dev,0,1,"mychar");
```

Suppose:

```text
Major = 240
Minor = 0
```

Kernel now knows:

```text
Major 240 -> my driver
Minor 0 -> device instance
```

---

# 7. Character Driver Registration

## Step 1

Allocate device number

```c
alloc_chrdev_region()
```

Kernel:

```text
Find free major number
Reserve it
Return dev_t
```

---

## Step 2

Initialize cdev

```c
cdev_init()
```

Kernel creates:

```text
struct cdev
```

which represents the character device object.

---

## Step 3

Register cdev

```c
cdev_add()
```

Now VFS knows:

```text
Major 240 -> my file_operations
```

---

# 8. Internal Kernel Objects

## cdev

```c
struct cdev {
    struct kobject kobj;
    struct module *owner;
    struct file_operations *ops;
};
```

Purpose:

```text
Represent Character Device
```

---

## file_operations

Most important structure.

```c
struct file_operations {
    int (*open)(...);
    int (*release)(...);
    ssize_t (*read)(...);
    ssize_t (*write)(...);
    long (*unlocked_ioctl)(...);
};
```

Contains callbacks.

---

# 9. Complete Driver Registration Flow

```text
module_init()
      |
      v
alloc_chrdev_region()
      |
      v
cdev_init()
      |
      v
cdev_add()
      |
      v
class_create()
      |
      v
device_create()
      |
      v
/dev/mychar created
```

---

# 10. What Happens During open()

User:

```c
open("/dev/mychar")
```

Kernel flow:

```text
sys_open()
   |
   v
do_sys_open()
   |
   v
vfs_open()
   |
   v
inode->i_cdev
   |
   v
file_operations->open()
```

Driver callback:

```c
static int my_open(struct inode *inode,
                   struct file *file)
{
    return 0;
}
```

---

# 11. What Happens During write()

User:

```c
write(fd, buf, 100);
```

Kernel:

```text
sys_write()
    |
    v
vfs_write()
    |
    v
file_operations->write()
```

Driver:

```c
static ssize_t my_write(...)
{
}
```

---

# 12. User Space vs Kernel Space

```text
User Space
-------------
0x0000...

Kernel Space
-------------
0xffff...
```

Direct access forbidden.

Wrong:

```c
memcpy(kernel_buf,user_buf,len);
```

May crash.

Correct:

```c
copy_from_user()
```

---

# 13. copy_from_user()

```c
copy_from_user(kbuf, ubuf, len);
```

Flow:

```text
User Buffer
      |
      v
copy_from_user()
      |
      v
Kernel Buffer
```

Handles:

* Page faults
* Invalid pointers
* Protection checks

---

# 14. Read Operation

Driver:

```c
static ssize_t my_read(
       struct file *file,
       char __user *buf,
       size_t len,
       loff_t *off)
{
    copy_to_user(buf,kbuf,len);

    return len;
}
```

Flow:

```text
Hardware
    |
    v
Kernel Buffer
    |
    v
copy_to_user()
    |
    v
User Buffer
```

---

# 15. File Structure

Every open creates:

```c
struct file
```

Contains:

```c
struct file {
    loff_t f_pos;
    struct file_operations *f_op;
    void *private_data;
};
```

Useful field:

```c
file->private_data
```

Stores device context.

---

# 16. Driver Private Data

```c
struct my_device {
    int value;
    struct mutex lock;
};
```

Open:

```c
file->private_data = mydev;
```

Read:

```c
struct my_device *dev;

dev = file->private_data;
```

---

# 17. IOCTL

Used for device-specific commands.

```c
ioctl(fd,CMD_SET_SPEED,115200);
```

Driver:

```c
static long my_ioctl(
        struct file *file,
        unsigned int cmd,
        unsigned long arg)
{
}
```

Used when:

```text
read/write insufficient
```

Examples:

* Set baudrate
* Enable sensor
* Configure DMA

---

# 18. Poll and Select

Applications:

```c
select()
poll()
epoll()
```

Driver callback:

```c
poll()
```

Purpose:

```text
Notify data availability
```

---

# 19. Blocking vs Non-Blocking

Blocking:

```text
read()
 waits
```

Non-blocking:

```text
read()
 returns immediately
```

Implementation:

```c
wait_event_interruptible()
```

and

```c
wake_up_interruptible()
```

---

# 20. Interrupt Integration

Hardware:

```text
Data Ready
```

Raises IRQ.

Flow:

```text
IRQ
 |
 v
ISR
 |
 v
wake_up()
 |
 v
read() unblocks
```

---

# 21. DMA Integration

Without DMA:

```text
Device
  |
CPU copies
  |
RAM
```

With DMA:

```text
Device
   |
 DMA Engine
   |
 RAM
```

CPU utilization reduced.

---

# 22. Character Driver Memory Allocation

Common APIs:

```c
kmalloc()
kzalloc()
devm_kzalloc()
vmalloc()
dma_alloc_coherent()
```

### kmalloc

```text
Physically contiguous
```

### vmalloc

```text
Virtually contiguous
```

### DMA

```text
DMA capable memory
```

---

# 23. Platform Driver + Character Driver

Most modern drivers:

```text
Device Tree
      |
Platform Driver
      |
Probe()
      |
Character Driver
      |
/dev node
```

Example:

```text
GPIO
I2C
SPI
UART
```

---

# 24. Complete Character Driver Skeleton

```c
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = my_open,
    .release = my_close,
    .read = my_read,
    .write = my_write,
    .unlocked_ioctl = my_ioctl,
};

static int __init drv_init(void)
{
    alloc_chrdev_region(&dev,0,1,"mychar");

    cdev_init(&cdev,&fops);

    cdev_add(&cdev,dev,1);

    class_create();

    device_create();

    return 0;
}
```

---

# 25. Internal Open Path (Kernel Deep Dive)

```text
open()
 |
sys_openat()
 |
do_filp_open()
 |
path_openat()
 |
do_open()
 |
vfs_open()
 |
chrdev_open()
 |
inode->i_cdev
 |
cdev->ops->open()
 |
driver_open()
```

This is a very common interview topic for Qualcomm, NVIDIA, Intel, AMD, and automotive Linux positions.

---

# Interview Questions

### Basic

1. What is a character driver?
2. Difference between block and character driver?
3. What is major number?
4. What is minor number?
5. What is cdev?
6. What is file_operations?
7. Why use copy_from_user()?

### Intermediate

1. How does open() reach the driver?
2. What happens inside cdev_add()?
3. Difference between kmalloc and vmalloc?
4. What is file->private_data?
5. How do you support multiple devices?

### Advanced

1. Explain VFS to character driver path.
2. How does chrdev_open() work internally?
3. How does Linux map major/minor to cdev?
4. How would you integrate DMA into a char driver?
5. Explain poll/select implementation.
6. Explain blocking read using wait queues.
7. How does a platform driver create a character device node?

For Qualcomm, NVIDIA, and BSP/Driver interviews, you should be able to explain the entire flow:

```text
Device Tree
    ↓
Platform Driver Probe
    ↓
alloc_chrdev_region()
    ↓
cdev_add()
    ↓
device_create()
    ↓
/dev node
    ↓
open()
    ↓
VFS
    ↓
file_operations
    ↓
Driver
    ↓
Registers / IRQ / DMA
    ↓
Hardware
```

This end-to-end architecture is one of the most frequently discussed Linux driver topics for engineers with 5–10 years of experience.
