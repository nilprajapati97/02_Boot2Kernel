// =============================================================================
// full_driver.c — Level 02: Full Char Driver (IOCTL + Poll + IRQ + Ring Buffer)
// =============================================================================
// Adds over Level 01:
//   1. ioctl()    — control interface (clear buffer, get status)
//   2. poll()     — async readiness notification (select/epoll support)
//   3. Simulated IRQ — interrupt handler waking up wait queue
//   4. Lock-free ring buffer — high-performance data path
//   5. Mutex for concurrent access safety
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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 02 — Full Char Driver: IOCTL + Poll + IRQ + Ring Buffer");
MODULE_VERSION("2.0");

/* ─── IOCTL definitions ──────────────────────────────────────────────────── */
#define MY_IOCTL_MAGIC       'k'
#define IOCTL_CLEAR_BUFFER   _IO(MY_IOCTL_MAGIC,  0)
#define IOCTL_GET_BUF_LEN    _IOR(MY_IOCTL_MAGIC, 1, int)
#define IOCTL_SET_IRQ_SIM    _IOW(MY_IOCTL_MAGIC, 2, int)

/* ─── Configuration ─────────────────────────────────────────────────────── */
#define DRIVER_NAME     "full_chrdev"
#define DEVICE_NAME     "MyAnilDev2"
#define CLASS_NAME      "full_class"
#define RING_SIZE       512           /* must be power of 2 */
#define SIMULATED_IRQ   19            /* software IRQ for simulation */

/* ─── Ring Buffer ────────────────────────────────────────────────────────── */
struct ring_buf {
    char          data[RING_SIZE];
    unsigned int  head;    /* write pointer */
    unsigned int  tail;    /* read pointer  */
    atomic_t      count;   /* bytes available */
};

static inline bool ring_full(struct ring_buf *rb)
{
    return atomic_read(&rb->count) == RING_SIZE;
}

static inline bool ring_empty(struct ring_buf *rb)
{
    return atomic_read(&rb->count) == 0;
}

static inline void ring_push(struct ring_buf *rb, char c)
{
    rb->data[rb->head & (RING_SIZE - 1)] = c;
    rb->head++;
    atomic_inc(&rb->count);
}

static inline char ring_pop(struct ring_buf *rb)
{
    char c = rb->data[rb->tail & (RING_SIZE - 1)];
    rb->tail++;
    atomic_dec(&rb->count);
    return c;
}

/* ─── Global State ───────────────────────────────────────────────────────── */
static dev_t            dev_num;
static struct cdev      my_cdev;
static struct class    *my_class;
static struct device   *my_device;
static struct ring_buf  ring;
static DEFINE_MUTEX(ring_mutex);
static DECLARE_WAIT_QUEUE_HEAD(read_wq);   /* tasks waiting for data */

/* ─── IRQ simulation: software interrupt triggers data_ready ─────────────── */
static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    char dummy[] = "IRQ_DATA";
    int i;

    mutex_lock(&ring_mutex);
    for (i = 0; i < sizeof(dummy) - 1 && !ring_full(&ring); i++)
        ring_push(&ring, dummy[i]);
    mutex_unlock(&ring_mutex);

    wake_up_interruptible(&read_wq);    /* unblock any waiting readers */
    pr_info("%s: IRQ %d handled, woke readers\n", DRIVER_NAME, irq);
    return IRQ_HANDLED;
}

/* ─── file_operations: open ──────────────────────────────────────────────── */
static int my_open(struct inode *inode, struct file *filp)
{
    pr_info("%s: opened\n", DRIVER_NAME);
    return 0;
}

static int my_release(struct inode *inode, struct file *filp)
{
    pr_info("%s: released\n", DRIVER_NAME);
    return 0;
}

/* ─── file_operations: read ──────────────────────────────────────────────── */
/*
 * Blocking read: sleeps until data is available in the ring buffer.
 * Uses wait_event_interruptible() to be awakened by IRQ handler.
 */
static ssize_t my_read(struct file *filp, char __user *ubuf,
                        size_t count, loff_t *ppos)
{
    ssize_t ret = 0;
    char    tmp[RING_SIZE];
    size_t  i;

    /* Block if ring is empty (unless O_NONBLOCK) */
    if (ring_empty(&ring)) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(read_wq, !ring_empty(&ring)))
            return -ERESTARTSYS;
    }

    mutex_lock(&ring_mutex);
    for (i = 0; i < count && !ring_empty(&ring); i++)
        tmp[i] = ring_pop(&ring);
    mutex_unlock(&ring_mutex);

    if (copy_to_user(ubuf, tmp, i))
        return -EFAULT;

    ret = (ssize_t)i;
    pr_info("%s: read %zd bytes\n", DRIVER_NAME, ret);
    return ret;
}

/* ─── file_operations: write ─────────────────────────────────────────────── */
static ssize_t my_write(struct file *filp, const char __user *ubuf,
                         size_t count, loff_t *ppos)
{
    char    tmp[RING_SIZE];
    size_t  to_copy = min(count, (size_t)RING_SIZE);
    size_t  i;

    if (copy_from_user(tmp, ubuf, to_copy))
        return -EFAULT;

    mutex_lock(&ring_mutex);
    for (i = 0; i < to_copy && !ring_full(&ring); i++)
        ring_push(&ring, tmp[i]);
    mutex_unlock(&ring_mutex);

    wake_up_interruptible(&read_wq);
    pr_info("%s: wrote %zu bytes\n", DRIVER_NAME, i);
    return (ssize_t)i;
}

/* ─── file_operations: ioctl ─────────────────────────────────────────────── */
/*
 * unlocked_ioctl: runs in process context with BKL removed.
 * _IO  → no data transfer
 * _IOR → kernel → user data (Read from kernel perspective)
 * _IOW → user → kernel data (Write to kernel)
 */
static long my_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int val;

    switch (cmd) {
    case IOCTL_CLEAR_BUFFER:
        mutex_lock(&ring_mutex);
        ring.head = ring.tail = 0;
        atomic_set(&ring.count, 0);
        mutex_unlock(&ring_mutex);
        pr_info("%s: ioctl CLEAR_BUFFER\n", DRIVER_NAME);
        break;

    case IOCTL_GET_BUF_LEN:
        val = atomic_read(&ring.count);
        if (copy_to_user((int __user *)arg, &val, sizeof(val)))
            return -EFAULT;
        break;

    case IOCTL_SET_IRQ_SIM:
        if (copy_from_user(&val, (int __user *)arg, sizeof(val)))
            return -EFAULT;
        /* In real driver: trigger/configure HW interrupt here */
        pr_info("%s: ioctl SET_IRQ_SIM val=%d\n", DRIVER_NAME, val);
        break;

    default:
        return -ENOTTY;   /* "not a typewriter" — standard for unknown ioctl */
    }
    return 0;
}

/* ─── file_operations: poll ──────────────────────────────────────────────── */
/*
 * poll() enables select()/epoll() support.
 * Must call poll_wait() to register the wait queue.
 * Return bitmask of EPOLLIN/EPOLLOUT based on actual readiness.
 */
static __poll_t my_poll(struct file *filp, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(filp, &read_wq, wait);  /* register: wake me when read_wq fires */

    if (!ring_empty(&ring))
        mask |= EPOLLIN | EPOLLRDNORM;   /* readable */

    if (!ring_full(&ring))
        mask |= EPOLLOUT | EPOLLWRNORM;  /* writable */

    return mask;
}

/* ─── file_operations table ──────────────────────────────────────────────── */
static const struct file_operations my_fops = {
    .owner          = THIS_MODULE,
    .open           = my_open,
    .release        = my_release,
    .read           = my_read,
    .write          = my_write,
    .unlocked_ioctl = my_ioctl,
    .poll           = my_poll,
};

/* ─── module_init ────────────────────────────────────────────────────────── */
static int __init full_chrdev_init(void)
{
    int ret;

    memset(&ring, 0, sizeof(ring));
    atomic_set(&ring.count, 0);

    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret) goto err_out;

    cdev_init(&my_cdev, &my_fops);
    my_cdev.owner = THIS_MODULE;
    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret) goto err_unreg;

    my_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(my_class)) { ret = PTR_ERR(my_class); goto err_cdev; }

    my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(my_device)) { ret = PTR_ERR(my_device); goto err_class; }

    /* Request shared IRQ (IRQF_SHARED requires matching cookie) */
    ret = request_irq(SIMULATED_IRQ, my_irq_handler,
                      IRQF_SHARED, DRIVER_NAME, &my_cdev);
    if (ret) {
        pr_warn("%s: IRQ %d request failed (simulation only): %d\n",
                DRIVER_NAME, SIMULATED_IRQ, ret);
        /* Non-fatal for simulation — continue loading */
        ret = 0;
    }

    pr_info("%s: /dev/%s ready\n", DRIVER_NAME, DEVICE_NAME);
    return 0;

err_class: class_destroy(my_class);
err_cdev:  cdev_del(&my_cdev);
err_unreg: unregister_chrdev_region(dev_num, 1);
err_out:   return ret;
}

/* ─── module_exit ────────────────────────────────────────────────────────── */
static void __exit full_chrdev_exit(void)
{
    free_irq(SIMULATED_IRQ, &my_cdev);
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(full_chrdev_init);
module_exit(full_chrdev_exit);
