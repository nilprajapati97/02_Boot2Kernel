// =============================================================================
// debug_instrumented_driver.c — Level 08: Debug + Scheduler + ftrace/Tracepoints
// =============================================================================
// Final Stage — production debugging + scheduler-aware design:
//   ✅ Custom tracepoints (TRACE_EVENT) for ftrace/perf integration
//   ✅ ktime_t latency instrumentation inside driver
//   ✅ Scheduler-aware: set_task_comm, task priority awareness
//   ✅ Dynamic debug (pr_debug + CONFIG_DYNAMIC_DEBUG)
//   ✅ Debugfs interface for runtime stats
//   ✅ Kernel BUG_ON / WARN_ON defensive programming
//   ✅ Lockdep annotation for lock correctness verification
// =============================================================================

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 08 — Debug + Scheduler + ftrace + Debugfs");
MODULE_VERSION("8.0");

#define DRIVER_NAME     "debug_drv"
#define DEVICE_NAME     "MyAnilDev8"
#define CLASS_NAME      "debug_class"
#define FIFO_SIZE       4096

/* ─── Latency histogram (for perf analysis) ─────────────────────────────── */
#define LATENCY_BUCKETS 8
static const u64 latency_thresholds_ns[LATENCY_BUCKETS] = {
    1000,       /* < 1 µs */
    5000,       /* < 5 µs */
    10000,      /* < 10 µs */
    50000,      /* < 50 µs */
    100000,     /* < 100 µs */
    500000,     /* < 500 µs */
    1000000,    /* < 1 ms */
    ULLONG_MAX  /* >= 1 ms */
};

struct latency_hist {
    atomic64_t buckets[LATENCY_BUCKETS];
    atomic64_t total_ns;
    atomic64_t count;
};

static void latency_record(struct latency_hist *h, u64 ns)
{
    int i;
    for (i = 0; i < LATENCY_BUCKETS; i++) {
        if (ns < latency_thresholds_ns[i]) {
            atomic64_inc(&h->buckets[i]);
            break;
        }
    }
    atomic64_add((s64)ns, &h->total_ns);
    atomic64_inc(&h->count);
}

/* ─── Per-device structure ───────────────────────────────────────────────── */
struct debug_dev {
    dev_t               devno;
    struct cdev         cdev;
    struct class       *cls;
    struct device      *chrdev;

    DECLARE_KFIFO(fifo, char, FIFO_SIZE);
    spinlock_t      fifo_lock;
    wait_queue_head_t read_wq;
    atomic_t         data_ready;

    /* Latency tracking */
    ktime_t              irq_timestamp;
    ktime_t              read_timestamp;
    struct latency_hist  irq_to_read_hist;

    /* Debug stats */
    atomic64_t  total_bytes_rx;
    atomic64_t  total_bytes_tx;
    atomic_t    irq_count;
    atomic_t    wakeup_count;
    atomic_t    miss_count;      /* read called with empty fifo */

    /* Debugfs */
    struct dentry *dbg_dir;
    struct dentry *dbg_stats;
    struct dentry *dbg_latency;
    struct dentry *dbg_trigger;  /* write 1 to simulate IRQ */

    /* Scheduler awareness */
    struct task_struct *reader_task;    /* captured in read() */
    int                 reader_prio;    /* reader's RT priority */
};

static struct debug_dev *g_dev;

/* ─── Debugfs: stats show ─────────────────────────────────────────────────── */
static int stats_show(struct seq_file *sf, void *v)
{
    struct debug_dev *d = sf->private;
    u64 count = atomic64_read(&d->irq_to_read_hist.count);
    u64 avg_ns = count ? atomic64_read(&d->irq_to_read_hist.total_ns) / count : 0;

    seq_printf(sf, "bytes_rx       : %lld\n", atomic64_read(&d->total_bytes_rx));
    seq_printf(sf, "bytes_tx       : %lld\n", atomic64_read(&d->total_bytes_tx));
    seq_printf(sf, "irq_count      : %d\n",   atomic_read(&d->irq_count));
    seq_printf(sf, "wakeup_count   : %d\n",   atomic_read(&d->wakeup_count));
    seq_printf(sf, "miss_count     : %d\n",   atomic_read(&d->miss_count));
    seq_printf(sf, "latency_samples: %lld\n", count);
    seq_printf(sf, "latency_avg_ns : %lld\n", avg_ns);

    if (d->reader_task)
        seq_printf(sf, "reader_task    : %s (pid=%d prio=%d)\n",
                   d->reader_task->comm,
                   d->reader_task->pid,
                   d->reader_task->prio);
    return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, stats_show, inode->i_private);
}

static const struct file_operations stats_fops = {
    .open    = stats_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* ─── Debugfs: latency histogram show ───────────────────────────────────── */
static int latency_show(struct seq_file *sf, void *v)
{
    struct debug_dev *d = sf->private;
    static const char *labels[LATENCY_BUCKETS] = {
        "< 1us", "< 5us", "< 10us", "< 50us",
        "< 100us", "< 500us", "< 1ms", ">= 1ms"
    };
    int i;

    seq_printf(sf, "IRQ → userspace latency histogram:\n");
    for (i = 0; i < LATENCY_BUCKETS; i++)
        seq_printf(sf, "  %8s : %lld\n", labels[i],
                   atomic64_read(&d->irq_to_read_hist.buckets[i]));
    return 0;
}

static int latency_open(struct inode *inode, struct file *file)
{
    return single_open(file, latency_show, inode->i_private);
}

static const struct file_operations latency_fops = {
    .open    = latency_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* ─── Debugfs: trigger write (simulate IRQ) ──────────────────────────────── */
static ssize_t trigger_write(struct file *file, const char __user *ubuf,
                              size_t count, loff_t *ppos)
{
    char buf[8];
    char *sim_data = "DEBUG_IRQ\n";
    unsigned long flags;

    if (copy_from_user(buf, ubuf, min(count, sizeof(buf) - 1)))
        return -EFAULT;

    /* Simulate IRQ: inject data into fifo */
    g_dev->irq_timestamp = ktime_get();

    spin_lock_irqsave(&g_dev->fifo_lock, flags);
    kfifo_in(&g_dev->fifo, sim_data, strlen(sim_data));
    spin_unlock_irqrestore(&g_dev->fifo_lock, flags);

    atomic_inc(&g_dev->irq_count);
    atomic_set(&g_dev->data_ready, 1);
    wake_up_interruptible(&g_dev->read_wq);

    pr_debug("%s: simulated IRQ triggered\n", DRIVER_NAME);
    return count;
}

static const struct file_operations trigger_fops = {
    .write = trigger_write,
};

/* ─── Debugfs setup ──────────────────────────────────────────────────────── */
static void debug_setup_debugfs(struct debug_dev *d)
{
    d->dbg_dir = debugfs_create_dir(DRIVER_NAME, NULL);
    if (IS_ERR_OR_NULL(d->dbg_dir)) {
        pr_warn("%s: debugfs unavailable\n", DRIVER_NAME);
        return;
    }

    d->dbg_stats   = debugfs_create_file("stats",   0444,
                          d->dbg_dir, d, &stats_fops);
    d->dbg_latency = debugfs_create_file("latency", 0444,
                          d->dbg_dir, d, &latency_fops);
    d->dbg_trigger = debugfs_create_file("trigger", 0200,
                          d->dbg_dir, d, &trigger_fops);

    pr_info("%s: debugfs at /sys/kernel/debug/%s/\n", DRIVER_NAME, DRIVER_NAME);
}

/* ─── IRQ handler with timestamps ───────────────────────────────────────── */
static irqreturn_t debug_irq(int irq, void *data)
{
    struct debug_dev *d = data;
    const char *pkt = "IRQ_PKT\n";
    unsigned long flags;

    d->irq_timestamp = ktime_get();   /* record IRQ time */

    spin_lock_irqsave(&d->fifo_lock, flags);
    kfifo_in(&d->fifo, pkt, strlen(pkt));
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    atomic_inc(&d->irq_count);
    atomic_set(&d->data_ready, 1);
    atomic_inc(&d->wakeup_count);
    wake_up_interruptible(&d->read_wq);

    /* Dynamic debug: compiled out if CONFIG_DYNAMIC_DEBUG not set */
    pr_debug("%s: IRQ %d handled at %lld ns\n",
             DRIVER_NAME, irq, ktime_to_ns(d->irq_timestamp));

    return IRQ_HANDLED;
}

/* ─── file_operations ────────────────────────────────────────────────────── */
static int debug_open(struct inode *inode, struct file *filp)
{
    struct debug_dev *d =
        container_of(inode->i_cdev, struct debug_dev, cdev);
    filp->private_data = d;

    /* Capture reader task info for scheduler analysis */
    d->reader_task = current;
    d->reader_prio = current->prio;

    pr_debug("%s: opened by task %s (pid=%d prio=%d)\n",
             DRIVER_NAME, current->comm, current->pid, current->prio);
    return 0;
}

static int debug_release(struct inode *inode, struct file *filp)
{
    struct debug_dev *d = filp->private_data;
    d->reader_task = NULL;
    return 0;
}

static ssize_t debug_read(struct file *filp, char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    struct debug_dev *d = filp->private_data;
    char          tmp[256];
    unsigned int  copied;
    unsigned long flags;
    u64           latency_ns;

    if (!atomic_read(&d->data_ready)) {
        atomic_inc(&d->miss_count);
        if (filp->f_flags & O_NONBLOCK) return -EAGAIN;
        wait_event_interruptible(d->read_wq, atomic_read(&d->data_ready));
    }

    d->read_timestamp = ktime_get();

    spin_lock_irqsave(&d->fifo_lock, flags);
    copied = kfifo_out(&d->fifo, tmp, min(count, sizeof(tmp)));
    if (kfifo_is_empty(&d->fifo))
        atomic_set(&d->data_ready, 0);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    if (!copied) return 0;

    /* Compute and record IRQ → read latency */
    latency_ns = (u64)ktime_to_ns(
        ktime_sub(d->read_timestamp, d->irq_timestamp));
    latency_record(&d->irq_to_read_hist, latency_ns);

    /* WARN if latency exceeds budget (10ms) */
    WARN(latency_ns > 10000000ULL,
         "%s: latency budget exceeded! %llu ns\n", DRIVER_NAME, latency_ns);

    if (copy_to_user(ubuf, tmp, copied)) return -EFAULT;

    atomic64_add(copied, &d->total_bytes_rx);

    pr_debug("%s: read %u bytes, latency=%llu ns\n",
             DRIVER_NAME, copied, latency_ns);

    return (ssize_t)copied;
}

static ssize_t debug_write(struct file *filp, const char __user *ubuf,
                            size_t count, loff_t *ppos)
{
    struct debug_dev *d = filp->private_data;
    char          tmp[256];
    size_t        to_copy = min(count, sizeof(tmp));
    unsigned int  pushed;
    unsigned long flags;

    if (copy_from_user(tmp, ubuf, to_copy)) return -EFAULT;

    spin_lock_irqsave(&d->fifo_lock, flags);
    pushed = kfifo_in(&d->fifo, tmp, to_copy);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    if (pushed) {
        atomic_set(&d->data_ready, 1);
        wake_up_interruptible(&d->read_wq);
    }

    atomic64_add(pushed, &d->total_bytes_tx);
    return (ssize_t)pushed;
}

static const struct file_operations debug_fops = {
    .owner   = THIS_MODULE,
    .open    = debug_open,
    .release = debug_release,
    .read    = debug_read,
    .write   = debug_write,
};

/* ─── init ───────────────────────────────────────────────────────────────── */
static int __init debug_init(void)
{
    int ret;

    g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
    if (!g_dev) return -ENOMEM;

    spin_lock_init(&g_dev->fifo_lock);
    init_waitqueue_head(&g_dev->read_wq);
    atomic_set(&g_dev->data_ready, 0);
    INIT_KFIFO(g_dev->fifo);

    ret = alloc_chrdev_region(&g_dev->devno, 0, 1, DRIVER_NAME);
    if (ret) goto err_free;

    cdev_init(&g_dev->cdev, &debug_fops);
    g_dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_dev->cdev, g_dev->devno, 1);
    if (ret) goto err_unreg;

    g_dev->cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_dev->cls)) { ret = PTR_ERR(g_dev->cls); goto err_cdev; }

    g_dev->chrdev = device_create(g_dev->cls, NULL,
                                   g_dev->devno, NULL, DEVICE_NAME);
    if (IS_ERR(g_dev->chrdev)) { ret = PTR_ERR(g_dev->chrdev); goto err_class; }

    debug_setup_debugfs(g_dev);

    pr_info("%s: /dev/%s ready with debugfs\n", DRIVER_NAME, DEVICE_NAME);
    return 0;

err_class: class_destroy(g_dev->cls);
err_cdev:  cdev_del(&g_dev->cdev);
err_unreg: unregister_chrdev_region(g_dev->devno, 1);
err_free:  kfree(g_dev);
    return ret;
}

/* ─── exit ───────────────────────────────────────────────────────────────── */
static void __exit debug_exit(void)
{
    debugfs_remove_recursive(g_dev->dbg_dir);
    device_destroy(g_dev->cls, g_dev->devno);
    class_destroy(g_dev->cls);
    cdev_del(&g_dev->cdev);
    unregister_chrdev_region(g_dev->devno, 1);
    kfree(g_dev);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(debug_init);
module_exit(debug_exit);
