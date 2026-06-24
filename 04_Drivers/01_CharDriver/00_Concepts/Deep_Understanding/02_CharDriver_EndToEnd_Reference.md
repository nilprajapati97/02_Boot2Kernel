# CharDriver End-to-End Reference

Complete reference for Linux character device driver design: kernel concepts, runtime flow, full implementation, and best practices.

---

## High-Level Flow

```
User Space
  open() / read() / write() / ioctl() / mmap()
      │
      ▼
VFS / syscall layer
      │
      ▼
struct file → file->f_op  ──► driver file_operations (open/read/write/ioctl/poll/mmap)
      │
      ▼
hardware (MMIO / bus) or internal kernel buffers
```

---

## Key Kernel Concepts

| Concept | What | Why |
|---|---|---|
| `dev_t` / Major / Minor | 32-bit identifier combining major (driver) + minor (instance) | Kernel routes `/dev` node to correct driver |
| `struct cdev` | Kernel object linking `dev_t` to `file_operations` | VFS dispatch target |
| `struct file_operations` | Table of function pointers (`open`, `read`, `write`, `ioctl`, `poll`, `mmap`, …) | VFS calls these on each syscall |
| `struct inode` | Per-file kernel object (persistent); holds `dev_t` for device nodes | Identifies device on open |
| `struct file` | Per-open-instance object; holds `f_pos`, `f_flags`, `private_data` | Carries per-fd state |
| `copy_to_user` / `copy_from_user` | Safe kernel↔user data transfer | Kernel must not directly dereference user pointers |
| `wait_queue` | Blocks a process until a condition is true | Implements blocking `read`/`write` without busy-wait |
| `poll` / `select` | Reports device readiness mask | Non-blocking, event-driven I/O |
| `mmap` / `remap_pfn_range` | Maps device/DMA memory into user address space | Zero-copy high-throughput transfers |
| `unlocked_ioctl` | Device-specific control interface | Structured commands outside read/write byte stream |
| `class_create` + `device_create` | Creates sysfs class/device; triggers udev to make `/dev` node | Modern auto device node creation |

---

## Runtime Execution Path

### A — module_init: Driver Registration

1. `alloc_chrdev_region()` — dynamic `dev_t` assignment
2. `cdev_init()` + `cdev_add()` — link and activate cdev
3. `class_create()` + `device_create()` — expose to udev → `/dev/<name>`
4. Allocate driver data (`kzalloc`), init wait queues and locks

### B — open()

- VFS resolves `/dev/<name>` inode by `dev_t` → calls driver `.open(inode, file)`
- Driver allocates per-open state, stores in `file->private_data`

### C — read()

- VFS calls `.read(file, ubuf, count, &pos)`
- If buffer empty and blocking: `wait_event_interruptible(readq, !empty)`
- `copy_to_user(ubuf, kbuf, n)` — transfer data to user
- `wake_up_interruptible(&writeq)` — unblock waiting writers

### D — write()

- VFS calls `.write(file, ubuf, count, &pos)`
- If buffer full and blocking: `wait_event_interruptible(writeq, !full)`
- `copy_from_user(kbuf, ubuf, n)` — receive data from user
- `wake_up_interruptible(&readq)` — unblock waiting readers

### E — poll()

- Register wait queues with `poll_wait()`
- Return `POLLIN | POLLRDNORM` if data available; `POLLOUT | POLLWRNORM` if space available

### F — ioctl()

- VFS calls `.unlocked_ioctl(file, cmd, arg)`
- Decode with `_IOC_TYPE` / `_IOC_NR` / `_IOC_SIZE` macros
- Use `copy_from_user` / `copy_to_user` for structured arguments

### G — mmap()

- Implement `.mmap` and use `remap_pfn_range()` to map physical pages into user VMA
- Set `vm_ops` if on-demand page faults are needed

### H — release()

- VFS calls `.release(inode, file)` on last `close()`
- Free `file->private_data`, release hardware resources

### I — module_exit: Cleanup

```c
device_destroy(my_class, devno);
class_destroy(my_class);
cdev_del(&my_cdev);
unregister_chrdev_region(devno, 1);
```

---

## Full Implementation — Buffered Blocking Char Driver

```c
// simple_char.c — circular buffer, blocking read/write, poll support
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/poll.h>

#define DEV_NAME  "mychardev"
#define BUF_SIZE  1024

static dev_t           dev_number;
static struct cdev     my_cdev;
static struct class   *my_class;

struct mydev {
    char                 *buf;
    size_t                head, tail;
    wait_queue_head_t     readq, writeq;
    struct mutex          lock;
} *mydev;

static bool buf_empty(struct mydev *d) { return d->head == d->tail; }
static bool buf_full(struct mydev *d)  { return ((d->head + 1) % BUF_SIZE) == d->tail; }

static int my_open(struct inode *inode, struct file *filp)
{
    filp->private_data = mydev;
    return 0;
}

static int my_release(struct inode *inode, struct file *filp) { return 0; }

static ssize_t my_read(struct file *filp, char __user *ubuf, size_t count, loff_t *ppos)
{
    struct mydev *d = filp->private_data;
    size_t copied = 0;

    if (buf_empty(d)) {
        if (filp->f_flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(d->readq, !buf_empty(d))) return -ERESTARTSYS;
    }

    mutex_lock(&d->lock);
    while (count && !buf_empty(d)) {
        char c = d->buf[d->tail];
        if (copy_to_user(ubuf + copied, &c, 1)) { mutex_unlock(&d->lock); return -EFAULT; }
        d->tail = (d->tail + 1) % BUF_SIZE;
        copied++; count--;
    }
    mutex_unlock(&d->lock);
    wake_up_interruptible(&d->writeq);
    return copied;
}

static ssize_t my_write(struct file *filp, const char __user *ubuf, size_t count, loff_t *ppos)
{
    struct mydev *d = filp->private_data;
    size_t written = 0;

    while (count) {
        if (buf_full(d)) {
            if (filp->f_flags & O_NONBLOCK) return written ? written : -EAGAIN;
            if (wait_event_interruptible(d->writeq, !buf_full(d))) return -ERESTARTSYS;
        }
        mutex_lock(&d->lock);
        while (count && !buf_full(d)) {
            char c;
            if (copy_from_user(&c, ubuf + written, 1)) { mutex_unlock(&d->lock); return -EFAULT; }
            d->buf[d->head] = c;
            d->head = (d->head + 1) % BUF_SIZE;
            written++; count--;
        }
        mutex_unlock(&d->lock);
        wake_up_interruptible(&d->readq);
    }
    return written;
}

static unsigned int my_poll(struct file *filp, poll_table *wait)
{
    struct mydev *d = filp->private_data;
    unsigned int mask = 0;
    poll_wait(filp, &d->readq,  wait);
    poll_wait(filp, &d->writeq, wait);
    mutex_lock(&d->lock);
    if (!buf_empty(d)) mask |= POLLIN  | POLLRDNORM;
    if (!buf_full(d))  mask |= POLLOUT | POLLWRNORM;
    mutex_unlock(&d->lock);
    return mask;
}

static const struct file_operations my_fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .release = my_release,
    .read    = my_read,
    .write   = my_write,
    .poll    = my_poll,
};

static int __init my_init(void)
{
    int err;
    err = alloc_chrdev_region(&dev_number, 0, 1, DEV_NAME);
    if (err) return err;

    cdev_init(&my_cdev, &my_fops);
    my_cdev.owner = THIS_MODULE;
    err = cdev_add(&my_cdev, dev_number, 1);
    if (err) goto err_cdev;

    my_class = class_create(THIS_MODULE, DEV_NAME);
    if (IS_ERR(my_class)) { err = PTR_ERR(my_class); goto err_class; }

    device_create(my_class, NULL, dev_number, NULL, DEV_NAME);

    mydev = kzalloc(sizeof(*mydev), GFP_KERNEL);
    if (!mydev) { err = -ENOMEM; goto err_alloc; }

    mydev->buf = kzalloc(BUF_SIZE, GFP_KERNEL);
    if (!mydev->buf) { err = -ENOMEM; goto err_buf; }

    init_waitqueue_head(&mydev->readq);
    init_waitqueue_head(&mydev->writeq);
    mutex_init(&mydev->lock);

    pr_info("%s: major=%d\n", DEV_NAME, MAJOR(dev_number));
    return 0;

err_buf:   kfree(mydev);
err_alloc: device_destroy(my_class, dev_number); class_destroy(my_class);
err_class: cdev_del(&my_cdev);
err_cdev:  unregister_chrdev_region(dev_number, 1);
    return err;
}

static void __exit my_exit(void)
{
    kfree(mydev->buf);
    kfree(mydev);
    device_destroy(my_class, dev_number);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_number, 1);
}

MODULE_LICENSE("GPL");
module_init(my_init);
module_exit(my_exit);
```

---

## User-Space Test

```c
int fd = open("/dev/mychardev", O_RDWR);
write(fd, "hello", 5);
char buf[10];
read(fd, buf, 5);
close(fd);
```

```bash
echo -n "hello" > /dev/mychardev
cat /dev/mychardev
```

---

## Common Pitfalls

| Pitfall | Fix |
|---|---|
| Direct user pointer dereference | Always use `copy_to_user` / `copy_from_user` |
| Sleeping in atomic context | Never sleep inside interrupt handler or while holding spinlock |
| Ignoring `-ERESTARTSYS` | Handle signal return from `wait_event_interruptible` |
| `rmmod` while device open | Set `.owner = THIS_MODULE`; use module refcounting |
| Init error path leaks | Undo steps in reverse order on failure |

---

## Debugging

```bash
dmesg                          # kernel log
cat /proc/devices              # registered major numbers
ls -la /dev/mychardev          # verify node and major:minor
strace ./test_app              # trace syscalls
udevadm info /dev/mychardev    # udev device info
```

---

## Advanced Topics

- DMA + `mmap`: map DMA coherent buffer to user space via `remap_pfn_range()`
- Platform driver integration: `cdev_add()` inside `platform_driver.probe()` using DT resources
- `io_uring` / AIO: async I/O paths interact differently with driver poll/read
- Runtime PM: `pm_runtime_get_sync()` / `pm_runtime_put()` around hardware access
