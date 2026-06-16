// =============================================================================
// basic_chardriver.c — Level 01: Basic Char Driver (Registration + VFS Flow)
// =============================================================================
// Demonstrates:
//   1. Dynamic major number allocation via alloc_chrdev_region()
//   2. cdev initialization and cdev_add()
//   3. device_create() for /dev entry via udev
//   4. file_operations: open, release, read, write
//   5. Clean module_exit teardown
// =============================================================================

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>          // file_operations, alloc_chrdev_region
#include <linux/cdev.h>        // cdev_init, cdev_add
#include <linux/device.h>      // class_create, device_create
#include <linux/uaccess.h>     // copy_to_user, copy_from_user
#include <linux/slab.h>        // kmalloc, kfree

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 01 — Basic Char Driver: Registration and VFS Flow");
MODULE_VERSION("1.0");

/* ─── Configuration ─────────────────────────────────────────────────────── */
#define DRIVER_NAME     "basic_chrdev"
#define DEVICE_NAME     "MyAnilDev"
#define CLASS_NAME      "basic_class"
#define BUFFER_SIZE     1024
#define MINOR_BASE      0
#define MINOR_COUNT     1

/* ─── Global State ───────────────────────────────────────────────────────── */
static dev_t            dev_num;          /* major:minor allocated by kernel  */
static struct cdev      my_cdev;          /* kernel char device object        */
static struct class    *my_class;         /* /sys/class/<class> entry         */
static struct device   *my_device;        /* /dev/<device> entry via udev     */
static char             kernel_buf[BUFFER_SIZE]; /* in-kernel data buffer     */
static size_t           buf_len = 0;      /* bytes currently in buffer        */

/* ─── file_operations: open ──────────────────────────────────────────────── */
static int my_open(struct inode *inode, struct file *filp)
{
    pr_info("%s: device opened (major=%d minor=%d)\n",
            DRIVER_NAME, imajor(inode), iminor(inode));
    return 0;
}

/* ─── file_operations: release ───────────────────────────────────────────── */
static int my_release(struct inode *inode, struct file *filp)
{
    pr_info("%s: device released\n", DRIVER_NAME);
    return 0;
}

/* ─── file_operations: read ──────────────────────────────────────────────── */
/*
 * VFS path:  user read() syscall
 *              → sys_read()
 *                → vfs_read()
 *                  → my_read()          ← we are here
 *
 * Rules:
 *  - *ppos tracks the current file position
 *  - Return bytes actually transferred, 0 for EOF, negative for error
 *  - copy_to_user() handles safe kernel→user copy + fault detection
 */
static ssize_t my_read(struct file *filp, char __user *ubuf,
                        size_t count, loff_t *ppos)
{
    size_t to_copy;

    if (*ppos >= buf_len)           /* EOF: nothing left to read */
        return 0;

    to_copy = min(count, buf_len - (size_t)*ppos);

    if (copy_to_user(ubuf, kernel_buf + *ppos, to_copy)) {
        pr_err("%s: copy_to_user failed\n", DRIVER_NAME);
        return -EFAULT;
    }

    *ppos += to_copy;
    pr_info("%s: read %zu bytes (pos=%lld)\n", DRIVER_NAME, to_copy, *ppos);
    return (ssize_t)to_copy;
}

/* ─── file_operations: write ─────────────────────────────────────────────── */
/*
 * VFS path:  user write() syscall
 *              → sys_write()
 *                → vfs_write()
 *                  → my_write()         ← we are here
 */
static ssize_t my_write(struct file *filp, const char __user *ubuf,
                         size_t count, loff_t *ppos)
{
    size_t to_copy = min(count, (size_t)(BUFFER_SIZE - 1));

    if (copy_from_user(kernel_buf, ubuf, to_copy)) {
        pr_err("%s: copy_from_user failed\n", DRIVER_NAME);
        return -EFAULT;
    }

    kernel_buf[to_copy] = '\0';
    buf_len = to_copy;
    *ppos   = to_copy;

    pr_info("%s: wrote %zu bytes: \"%s\"\n", DRIVER_NAME, to_copy, kernel_buf);
    return (ssize_t)to_copy;
}

/* ─── file_operations table ──────────────────────────────────────────────── */
static const struct file_operations my_fops = {
    .owner   = THIS_MODULE,
    .open    = my_open,
    .release = my_release,
    .read    = my_read,
    .write   = my_write,
};

/* ─── module_init ────────────────────────────────────────────────────────── */
/*
 * Sequence:
 *   1. alloc_chrdev_region()  → kernel assigns major number dynamically
 *   2. cdev_init() + cdev_add() → register file_operations with kernel VFS
 *   3. class_create()         → create /sys/class/<class>
 *   4. device_create()        → udev creates /dev/<device>
 */
static int __init basic_chrdev_init(void)
{
    int ret;

    /* Step 1 — Dynamic device number allocation */
    ret = alloc_chrdev_region(&dev_num, MINOR_BASE, MINOR_COUNT, DRIVER_NAME);
    if (ret < 0) {
        pr_err("%s: alloc_chrdev_region failed (%d)\n", DRIVER_NAME, ret);
        return ret;
    }
    pr_info("%s: allocated major=%d minor=%d\n",
            DRIVER_NAME, MAJOR(dev_num), MINOR(dev_num));

    /* Step 2 — Initialize cdev and bind file_operations */
    cdev_init(&my_cdev, &my_fops);
    my_cdev.owner = THIS_MODULE;

    ret = cdev_add(&my_cdev, dev_num, MINOR_COUNT);
    if (ret < 0) {
        pr_err("%s: cdev_add failed (%d)\n", DRIVER_NAME, ret);
        goto err_unreg;
    }

    /* Step 3 — Create sysfs class */
    my_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(my_class)) {
        ret = PTR_ERR(my_class);
        pr_err("%s: class_create failed (%d)\n", DRIVER_NAME, ret);
        goto err_cdev;
    }

    /* Step 4 — Create device node (/dev/MyAnilDev) */
    my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(my_device)) {
        ret = PTR_ERR(my_device);
        pr_err("%s: device_create failed (%d)\n", DRIVER_NAME, ret);
        goto err_class;
    }

    pr_info("%s: /dev/%s created successfully\n", DRIVER_NAME, DEVICE_NAME);
    return 0;

err_class:
    class_destroy(my_class);
err_cdev:
    cdev_del(&my_cdev);
err_unreg:
    unregister_chrdev_region(dev_num, MINOR_COUNT);
    return ret;
}

/* ─── module_exit ────────────────────────────────────────────────────────── */
/* Reverse of init — always clean up in reverse order */
static void __exit basic_chrdev_exit(void)
{
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, MINOR_COUNT);
    pr_info("%s: module unloaded\n", DRIVER_NAME);
}

module_init(basic_chrdev_init);
module_exit(basic_chrdev_exit);
