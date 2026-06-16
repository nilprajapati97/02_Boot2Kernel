// =============================================================================
// integrated_driver.c — Level 03: Full Integrated Production Char Driver
// =============================================================================
// All features combined in production-grade structure:
//   ✅ Dynamic device number allocation
//   ✅ ioctl (clear, status, config)
//   ✅ poll/select/epoll support
//   ✅ ISR top-half + threaded bottom-half (IRQF_ONESHOT)
//   ✅ Wait queue for blocking read
//   ✅ Ring buffer (lock-free style with atomic operations)
//   ✅ Mutex for critical sections
//   ✅ Proper init + teardown with goto error handling
//   ✅ Per-device private structure (scalable multi-instance pattern)
// =============================================================================

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 03 — Integrated Production Char Driver");
MODULE_VERSION("3.0");

/* ─── IOCTL interface ────────────────────────────────────────────────────── */
#define MY_MAGIC            'A'
#define IOCTL_RESET         _IO(MY_MAGIC,  0)
#define IOCTL_GET_STATUS    _IOR(MY_MAGIC, 1, struct dev_status)
#define IOCTL_SET_THRESHOLD _IOW(MY_MAGIC, 2, unsigned int)

struct dev_status {
    unsigned int bytes_in_buf;
    unsigned int total_reads;
    unsigned int total_writes;
    unsigned int total_irqs;
};

/* ─── Configuration ─────────────────────────────────────────────────────── */
#define DRIVER_NAME     "intg_chrdev"
#define DEVICE_NAME     "MyAnilDev3"
#define CLASS_NAME      "intg_class"
#define RING_SIZE       4096          /* power-of-2 */
#define IRQ_NUM         20

/* ─── Per-device structure (production pattern: no bare globals) ─────────── */
struct my_dev {
    /* Device registration */
    dev_t           devno;
    struct cdev     cdev;
    struct class   *cls;
    struct device  *dev;

    /* Data ring buffer */
    char            ring[RING_SIZE];
    unsigned int    head;
    unsigned int    tail;
    atomic_t        count;
    spinlock_t      ring_lock;   /* spinlock for IRQ context */

    /* Synchronization */
    struct mutex    io_mutex;
    wait_queue_head_t read_wq;
    wait_queue_head_t write_wq;

    /* Stats */
    atomic_t        total_reads;
    atomic_t        total_writes;
    atomic_t        total_irqs;

    /* Config */
    unsigned int    threshold;   /* bytes before signaling read readiness */
};

static struct my_dev *g_dev;    /* single device instance */

/* ─── Ring buffer helpers ────────────────────────────────────────────────── */
#define RING_MASK  (RING_SIZE - 1)

static unsigned int ring_avail(struct my_dev *d)
{
    return (unsigned int)atomic_read(&d->count);
}

static unsigned int ring_free(struct my_dev *d)
{
    return RING_SIZE - ring_avail(d);
}

static void ring_write_byte(struct my_dev *d, char c)
{
    d->ring[d->head & RING_MASK] = c;
    d->head++;
    atomic_inc(&d->count);
}

static char ring_read_byte(struct my_dev *d)
{
    char c = d->ring[d->tail & RING_MASK];
    d->tail++;
    atomic_dec(&d->count);
    return c;
}

/* ─── ISR: Top-half — runs in hardirq context ────────────────────────────── */
static irqreturn_t my_irq_top(int irq, void *data)
{
    /* Minimal work: just schedule threaded handler */
    return IRQ_WAKE_THREAD;
}

/* ─── ISR: Bottom-half — threaded, can use mutex ────────────────────────── */
static irqreturn_t my_irq_bottom(int irq, void *data)
{
    struct my_dev *d = (struct my_dev *)data;
    const char    *irq_data = "IRQ_PKT\n";
    unsigned long  flags;
    int            i;

    spin_lock_irqsave(&d->ring_lock, flags);
    for (i = 0; irq_data[i] && ring_free(d) > 0; i++)
        ring_write_byte(d, irq_data[i]);
    spin_unlock_irqrestore(&d->ring_lock, flags);

    atomic_inc(&d->total_irqs);
    wake_up_interruptible(&d->read_wq);
    return IRQ_HANDLED;
}

/* ─── open / release ─────────────────────────────────────────────────────── */
static int my_open(struct inode *inode, struct file *filp)
{
    struct my_dev *d = container_of(inode->i_cdev, struct my_dev, cdev);
    filp->private_data = d;       /* attach per-device data to file */
    pr_info("%s: opened\n", DRIVER_NAME);
    return 0;
}

static int my_release(struct inode *inode, struct file *filp)
{
    pr_info("%s: released\n", DRIVER_NAME);
    return 0;
}

/* ─── read ───────────────────────────────────────────────────────────────── */
static ssize_t my_read(struct file *filp, char __user *ubuf,
                        size_t count, loff_t *ppos)
{
    struct my_dev *d = filp->private_data;
    char    tmp[256];
    size_t  i = 0;
    unsigned long flags;

    if (ring_avail(d) == 0) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(d->read_wq,
                ring_avail(d) >= d->threshold))
            return -ERESTARTSYS;
    }

    mutex_lock(&d->io_mutex);
    spin_lock_irqsave(&d->ring_lock, flags);
    while (i < count && i < sizeof(tmp) && ring_avail(d) > 0)
        tmp[i++] = ring_read_byte(d);
    spin_unlock_irqrestore(&d->ring_lock, flags);
    mutex_unlock(&d->io_mutex);

    if (copy_to_user(ubuf, tmp, i))
        return -EFAULT;

    atomic_inc(&d->total_reads);
    wake_up_interruptible(&d->write_wq);   /* space freed — wake writers */
    return (ssize_t)i;
}

/* ─── write ──────────────────────────────────────────────────────────────── */
static ssize_t my_write(struct file *filp, const char __user *ubuf,
                         size_t count, loff_t *ppos)
{
    struct my_dev *d = filp->private_data;
    char    tmp[256];
    size_t  to_copy = min(count, sizeof(tmp));
    size_t  i = 0;
    unsigned long flags;

    if (ring_free(d) == 0) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(d->write_wq, ring_free(d) > 0))
            return -ERESTARTSYS;
    }

    if (copy_from_user(tmp, ubuf, to_copy))
        return -EFAULT;

    mutex_lock(&d->io_mutex);
    spin_lock_irqsave(&d->ring_lock, flags);
    while (i < to_copy && ring_free(d) > 0)
        ring_write_byte(d, tmp[i++]);
    spin_unlock_irqrestore(&d->ring_lock, flags);
    mutex_unlock(&d->io_mutex);

    atomic_inc(&d->total_writes);
    wake_up_interruptible(&d->read_wq);
    return (ssize_t)i;
}

/* ─── ioctl ──────────────────────────────────────────────────────────────── */
static long my_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct my_dev    *d = filp->private_data;
    struct dev_status st;
    unsigned int      thr;
    unsigned long     flags;

    switch (cmd) {
    case IOCTL_RESET:
        spin_lock_irqsave(&d->ring_lock, flags);
        d->head = d->tail = 0;
        atomic_set(&d->count, 0);
        spin_unlock_irqrestore(&d->ring_lock, flags);
        break;

    case IOCTL_GET_STATUS:
        st.bytes_in_buf  = ring_avail(d);
        st.total_reads   = (unsigned int)atomic_read(&d->total_reads);
        st.total_writes  = (unsigned int)atomic_read(&d->total_writes);
        st.total_irqs    = (unsigned int)atomic_read(&d->total_irqs);
        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        break;

    case IOCTL_SET_THRESHOLD:
        if (copy_from_user(&thr, (void __user *)arg, sizeof(thr)))
            return -EFAULT;
        if (thr > RING_SIZE)
            return -EINVAL;
        d->threshold = thr;
        break;

    default:
        return -ENOTTY;
    }
    return 0;
}

/* ─── poll ───────────────────────────────────────────────────────────────── */
static __poll_t my_poll(struct file *filp, poll_table *wait)
{
    struct my_dev *d = filp->private_data;
    __poll_t mask = 0;

    poll_wait(filp, &d->read_wq,  wait);
    poll_wait(filp, &d->write_wq, wait);

    if (ring_avail(d) >= d->threshold) mask |= EPOLLIN  | EPOLLRDNORM;
    if (ring_free(d) > 0)              mask |= EPOLLOUT | EPOLLWRNORM;

    return mask;
}

/* ─── file_operations ────────────────────────────────────────────────────── */
static const struct file_operations my_fops = {
    .owner          = THIS_MODULE,
    .open           = my_open,
    .release        = my_release,
    .read           = my_read,
    .write          = my_write,
    .unlocked_ioctl = my_ioctl,
    .poll           = my_poll,
};

/* ─── init ───────────────────────────────────────────────────────────────── */
static int __init intg_init(void)
{
    int ret;

    g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
    if (!g_dev) return -ENOMEM;

    /* Init synchronization primitives */
    spin_lock_init(&g_dev->ring_lock);
    mutex_init(&g_dev->io_mutex);
    init_waitqueue_head(&g_dev->read_wq);
    init_waitqueue_head(&g_dev->write_wq);
    atomic_set(&g_dev->count, 0);
    atomic_set(&g_dev->total_reads, 0);
    atomic_set(&g_dev->total_writes, 0);
    atomic_set(&g_dev->total_irqs, 0);
    g_dev->threshold = 1;

    ret = alloc_chrdev_region(&g_dev->devno, 0, 1, DRIVER_NAME);
    if (ret) goto err_free;

    cdev_init(&g_dev->cdev, &my_fops);
    g_dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_dev->cdev, g_dev->devno, 1);
    if (ret) goto err_unreg;

    g_dev->cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_dev->cls)) { ret = PTR_ERR(g_dev->cls); goto err_cdev; }

    g_dev->dev = device_create(g_dev->cls, NULL, g_dev->devno, NULL, DEVICE_NAME);
    if (IS_ERR(g_dev->dev)) { ret = PTR_ERR(g_dev->dev); goto err_class; }

    /* Threaded IRQ: top-half fast, bottom-half can sleep */
    ret = request_threaded_irq(IRQ_NUM,
            my_irq_top, my_irq_bottom,
            IRQF_SHARED | IRQF_ONESHOT, DRIVER_NAME, g_dev);
    if (ret) {
        pr_warn("%s: IRQ registration failed: %d (continuing)\n", DRIVER_NAME, ret);
        ret = 0;
    }

    pr_info("%s: /dev/%s ready (major=%d)\n",
            DRIVER_NAME, DEVICE_NAME, MAJOR(g_dev->devno));
    return 0;

err_class: class_destroy(g_dev->cls);
err_cdev:  cdev_del(&g_dev->cdev);
err_unreg: unregister_chrdev_region(g_dev->devno, 1);
err_free:  kfree(g_dev);
    return ret;
}

/* ─── exit ───────────────────────────────────────────────────────────────── */
static void __exit intg_exit(void)
{
    free_irq(IRQ_NUM, g_dev);
    device_destroy(g_dev->cls, g_dev->devno);
    class_destroy(g_dev->cls);
    cdev_del(&g_dev->cdev);
    unregister_chrdev_region(g_dev->devno, 1);
    kfree(g_dev);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(intg_init);
module_exit(intg_exit);
