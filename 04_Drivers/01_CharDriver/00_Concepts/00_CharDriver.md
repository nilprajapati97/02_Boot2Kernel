# Character Driver in Linux

A **character driver** allows user-space applications to interact with hardware or kernel functionality **byte-by-byte** using standard file operations (`open`, `read`, `write`, `ioctl`, `close`).

Examples: UART driver, I2C/SPI driver, GPIO driver, `/dev/null`.

---

## End-to-End Flow

### 1. Driver Registration

Every char driver must register itself with the kernel using `alloc_chrdev_region()`, which allocates a **major** (driver type) and **minor** (device instance) number.

```c
alloc_chrdev_region(&dev_num, 0, 1, "mychardev");
// dev_num contains (major, minor)
```

---

### 2. Create and Initialize cdev

Linux represents char drivers using `struct cdev`, linked to file operations.

```c
cdev_init(&my_cdev, &fops);
cdev_add(&my_cdev, dev_num, 1);
```

---

### 3. File Operations

```c
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = my_open,
    .release        = my_release,
    .read           = my_read,
    .write          = my_write,
    .unlocked_ioctl = my_ioctl,
};

static int my_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Device opened\n");
    return 0;
}

static int my_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Device closed\n");
    return 0;
}

static ssize_t my_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
    char kbuf[20] = "Hello User\n";
    copy_to_user(buf, kbuf, strlen(kbuf));   // Kernel → User
    return strlen(kbuf);
}

static ssize_t my_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
    char kbuf[100];
    copy_from_user(kbuf, buf, len);          // User → Kernel
    printk(KERN_INFO "User wrote: %s\n", kbuf);
    return len;
}
```

---

### 4. Device Node Creation

Creates `/dev/mychardev` via udev.

```c
class_create(THIS_MODULE, "my_class");
device_create(my_class, NULL, dev_num, NULL, "mychardev");
```

---

### 5. User-Space Access

```c
int fd = open("/dev/mychardev", O_RDWR);
write(fd, "LinuxDriver", 11);
char buf[50];
read(fd, buf, sizeof(buf));
printf("Got: %s", buf);
close(fd);
```

---

### 6. Driver Exit / Cleanup

```c
device_destroy(my_class, dev_num);
class_destroy(my_class);
cdev_del(&my_cdev);
unregister_chrdev_region(dev_num, 1);
```

---

## Flow Diagram

```
┌─────────────┐
│  User App   │  open / read / write / ioctl
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ /dev/entry  │  ← created by device_create()
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  VFS Layer  │  calls file_operations
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ Char Driver │  open / read / write / ioctl handlers
└──────┬──────┘
       │
       ▼
┌─────────────┐
│  Hardware / │
│ Kernel Data │
└─────────────┘
```

---

## Internal Execution Path

1. `open()`  → VFS resolves `/dev/mychardev` → calls `my_open()`
2. `read()`  → VFS forwards to `my_read()`  → driver copies data to user
3. `write()` → VFS forwards to `my_write()` → driver copies data from user
4. `ioctl()` → driver executes control commands
5. `close()` → `my_release()` is called

---

## Key Interview Points

| Topic | Detail |
|---|---|
| Major vs Minor | Major = driver type, Minor = device instance |
| `copy_to_user` / `copy_from_user` | Safe kernel ↔ user space data transfer |
| Blocking vs Non-Blocking IO | Use `wait_queue`, `poll`, `select` |
| Concurrency | Use `spinlock` or `mutex` |
| Char vs Block Driver | Char = sequential (UART), Block = random access (HDD) |
