# Design: Basic Char Driver — Registration & VFS Flow
## Level 01 | End-to-End Deep Design from Scratch

---

## 1. What is a Character Device?

A **character device** is a Linux kernel abstraction that allows user-space programs to communicate with hardware (or virtual devices) through a **file interface** — `/dev/<name>`.

Unlike block devices (disks), char devices transfer data **byte-by-byte, sequentially**, with no internal buffering at the VFS layer.

Examples: `/dev/tty`, `/dev/urandom`, `/dev/input/event0`, Qualcomm sensor hubs.

---

## 2. The Big Picture — Full Stack Flow

```
User Space                     Kernel Space
─────────────────────────────────────────────────────
  open("/dev/MyAnilDev")
       │
       ▼
  C library (glibc)  ──→  sys_open()  [syscall table]
                               │
                               ▼
                          vfs_open()  [Virtual File System]
                               │
                        Looks up inode in /dev/
                        Inode holds: major, minor, type
                               │
                               ▼
                          chrdev_open()  [fs/char_dev.c]
                          Finds cdev via chrdevs[] map
                               │
                               ▼
                          my_open()   ← YOUR DRIVER CODE
```

Every `read()`, `write()`, `ioctl()` follows the same path through VFS → your `file_operations`.

---

## 3. Device Number: major:minor

A device number is a 32-bit value `dev_t`:
- **Upper 12 bits** = major number → identifies the *driver*
- **Lower 20 bits** = minor number → identifies the *instance*

```
dev_t = MKDEV(major, minor)

Example: major=245, minor=0 → dev_t = (245 << 20) | 0
```

### Static vs Dynamic Allocation

| Method | API | Risk |
|--------|-----|------|
| Static | `register_chrdev_region(dev, count, name)` | Major conflict with other drivers |
| Dynamic | `alloc_chrdev_region(&dev, baseminor, count, name)` | Kernel picks free major safely |

**Always prefer dynamic allocation** in production drivers.

---

## 4. The `cdev` Structure — Kernel's Device Object

`struct cdev` is the kernel's internal representation of a char device:

```c
struct cdev {
    struct kobject          kobj;       // sysfs integration
    struct module          *owner;      // THIS_MODULE reference
    const struct file_operations *ops;  // YOUR function table
    struct list_head        list;       // internal kernel list
    dev_t                   dev;        // first device number
    unsigned int            count;      // number of minors
};
```

### Registration Sequence

```
alloc_chrdev_region()
        │
        ▼
   dev_num = (major << 20) | minor
        │
        ▼
cdev_init(&my_cdev, &my_fops)   ← binds file_operations
        │
        ▼
cdev_add(&my_cdev, dev_num, 1)  ← inserts into chrdevs[] hash table
        │
        ▼
Kernel VFS can now route I/O to your driver
```

Internally, `cdev_add()` calls `kobj_map()` which stores the cdev in a radix-tree keyed by `dev_t`. When VFS opens `/dev/MyAnilDev`, it extracts `major:minor` from the inode, looks up this map, and gets your `file_operations`.

---

## 5. The `file_operations` Structure

This is the **dispatch table** between VFS and your driver:

```c
struct file_operations {
    struct module *owner;
    loff_t (*llseek)   (struct file *, loff_t, int);
    ssize_t (*read)    (struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write)   (struct file *, const char __user *, size_t, loff_t *);
    int     (*open)    (struct inode *, struct file *);
    int     (*release) (struct inode *, struct file *);
    long    (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
    __poll_t (*poll)   (struct file *, struct poll_table_struct *);
    /* ... more ... */
};
```

**Which fields must be set?**
- `owner` — always `THIS_MODULE` (prevents module unload during active use)
- `open` / `release` — lifecycle management
- `read` / `write` — data transfer
- Rest → defaults to kernel generic implementations

---

## 6. sysfs / udev: How `/dev` Entry is Created

The kernel itself does **not** create `/dev` entries — udev (userspace) does.

```
class_create(THIS_MODULE, "basic_class")
        │
        ▼
  Creates /sys/class/basic_class/
        │
        ▼
device_create(my_class, NULL, dev_num, NULL, "MyAnilDev")
        │
        ▼
  Creates /sys/class/basic_class/MyAnilDev/
  with uevent containing MAJOR, MINOR, DEVNAME
        │
        ▼
udevd receives uevent  →  creates /dev/MyAnilDev
```

---

## 7. Data Transfer: `copy_to_user` / `copy_from_user`

### Why not use `memcpy`?

User-space pointers are **virtual addresses in a different address space**. Direct dereference is **undefined behavior** and a security vulnerability.

`copy_from_user()` and `copy_to_user()`:
1. Verify the user pointer is valid (not kernel address, in bounds)
2. Handle page faults gracefully (EFAULT if page not present)
3. On ARM64/x86: use hardware access control (PAN/SMAP) to detect bugs

```c
/* Kernel ← User */
if (copy_from_user(kernel_buf, user_ptr, count))
    return -EFAULT;

/* Kernel → User */
if (copy_to_user(user_ptr, kernel_buf, count))
    return -EFAULT;
```

Return value: **0 on success**, bytes NOT copied on failure.

---

## 8. The `loff_t *ppos` — File Position

`ppos` is the kernel-maintained file offset (like `fseek` position). It is stored per open file description (`struct file`), not per inode.

```c
/* Advance position after successful read */
*ppos += bytes_read;
return bytes_read;

/* Signal EOF */
if (*ppos >= buf_len)
    return 0;
```

---

## 9. module_init / module_exit Lifecycle

```
insmod basic_chardriver.ko
        │
        ▼
  basic_chrdev_init()
  ├─ alloc_chrdev_region()
  ├─ cdev_init() + cdev_add()
  ├─ class_create()
  └─ device_create()      → /dev/MyAnilDev appears

  ... driver active, VFS routes I/O ...

rmmod basic_chardriver
        │
        ▼
  basic_chrdev_exit()    (REVERSE ORDER mandatory)
  ├─ device_destroy()    → udev removes /dev/MyAnilDev
  ├─ class_destroy()
  ├─ cdev_del()
  └─ unregister_chrdev_region()
```

**Reverse order is critical**: destroying class before device would leave a dangling sysfs entry.

---

## 10. Kernel Error Handling Pattern

```c
/* Standard kernel error-path pattern using goto */
ret = step_A();
if (ret) goto err_A;

ret = step_B();
if (ret) goto err_B;

return 0;

err_B: undo_step_A();
err_A: return ret;
```

This ensures **no resource leaks** on partial failure — a strict requirement in kernel code where OOM conditions are real.

---

## 11. How to Build and Test

### Makefile

```makefile
obj-m := basic_chardriver.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
```

### Test Sequence

```bash
# Load module
sudo insmod basic_chardriver.ko

# Verify device created
ls -la /dev/MyAnilDev
cat /proc/devices | grep basic

# Write to device
echo "Hello Kernel" | sudo tee /dev/MyAnilDev

# Read from device
sudo cat /dev/MyAnilDev

# Check kernel log
dmesg | tail -20

# Unload module
sudo rmmod basic_chardriver
```

---

## 12. Common Mistakes and Fixes

| Mistake | Symptom | Fix |
|---------|---------|-----|
| Forgetting `cdev_del()` in exit | Kernel oops on rmmod | Always mirror init sequence in exit |
| Using `memcpy` instead of `copy_from_user` | Security hole, possible crash | Always use safe copy functions |
| Not checking `IS_ERR()` on `class_create` | Null pointer dereference | Every pointer return must be checked |
| Wrong cleanup order | sysfs/udev inconsistency | Always teardown in reverse init order |
| Missing `MODULE_LICENSE("GPL")` | Module not loadable | Always declare license |

---

## Summary

| Concept | API |
|---------|-----|
| Allocate device number | `alloc_chrdev_region()` |
| Register char device | `cdev_init()` + `cdev_add()` |
| Create sysfs class | `class_create()` |
| Create /dev node | `device_create()` |
| Safe user↔kernel copy | `copy_to_user()` / `copy_from_user()` |
| Module entry/exit | `module_init()` / `module_exit()` |
