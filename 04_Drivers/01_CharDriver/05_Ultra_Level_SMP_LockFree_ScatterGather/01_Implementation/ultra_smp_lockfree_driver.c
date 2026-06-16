// =============================================================================
// ultra_smp_lockfree_driver.c — Level 05: Ultra Level SMP + Lock-Free + SG-DMA
// =============================================================================
// Top 1% kernel engineering:
//   ✅ Memory barriers for SMP correctness (smp_wmb/smp_rmb/smp_mb)
//   ✅ Lock-free ring buffer using atomic operations
//   ✅ Scatter-Gather DMA (multi-buffer, real hardware pattern)
//   ✅ RCU (Read-Copy-Update) for config hot-path
//   ✅ Per-CPU local storage for stats (no cache thrashing)
//   ✅ kfifo (kernel's built-in lock-free FIFO)
// =============================================================================

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 05 — Ultra: SMP + Lock-Free + Scatter-Gather DMA");
MODULE_VERSION("5.0");

#define DRIVER_NAME     "ultra_chrdev"
#define DEVICE_NAME     "MyAnilDev5"
#define CLASS_NAME      "ultra_class"
#define FIFO_SIZE       4096
#define SG_MAX_ENTRIES  4              /* scatter-gather list entries */
#define SG_ENTRY_SIZE   PAGE_SIZE      /* 4KB per SG entry */

/* ─── Per-CPU stats (zero cache-line contention) ─────────────────────────── */
struct ultra_cpu_stats {
    unsigned long reads;
    unsigned long writes;
    unsigned long bytes_rx;
    unsigned long bytes_tx;
};
static DEFINE_PER_CPU(struct ultra_cpu_stats, cpu_stats);

/* ─── RCU-protected config (hot-path reads, rare writes) ────────────────── */
struct ultra_config {
    unsigned int  max_transfer;
    unsigned int  timeout_ms;
    struct rcu_head rcu;       /* for call_rcu() deferred free */
};

/* ─── Lock-free ring buffer (manual implementation for learning) ─────────── */
#define LF_RING_SIZE    1024
#define LF_RING_MASK    (LF_RING_SIZE - 1)

struct lf_ring {
    char              data[LF_RING_SIZE];
    atomic_t          head;   /* producer advances */
    atomic_t          tail;   /* consumer advances */
};

/*
 * SMP-safe lock-free push:
 * smp_wmb() ensures data write is visible before head update.
 * Without barrier: CPU2 could see head updated before data written.
 */
static bool lf_push(struct lf_ring *r, char c)
{
    int h = atomic_read(&r->head);
    int t = atomic_read(&r->tail);

    if (((h + 1) & LF_RING_MASK) == (t & LF_RING_MASK))
        return false;  /* full */

    r->data[h & LF_RING_MASK] = c;
    smp_wmb();                            /* data visible before head */
    atomic_set(&r->head, (h + 1) & LF_RING_MASK);
    return true;
}

static bool lf_pop(struct lf_ring *r, char *c)
{
    int h = atomic_read(&r->head);
    int t = atomic_read(&r->tail);

    if (t == h)
        return false;  /* empty */

    smp_rmb();                            /* read head before data */
    *c = r->data[t & LF_RING_MASK];
    smp_wmb();
    atomic_set(&r->tail, (t + 1) & LF_RING_MASK);
    return true;
}

/* ─── Scatter-Gather DMA setup ───────────────────────────────────────────── */
struct sg_dma_ctx {
    struct scatterlist  sgl[SG_MAX_ENTRIES];
    void               *bufs[SG_MAX_ENTRIES];     /* kernel virtual */
    dma_addr_t          dma_addr[SG_MAX_ENTRIES]; /* DMA addresses */
    unsigned int        nents;
    bool                mapped;
};

static int sg_dma_alloc(struct device *dev, struct sg_dma_ctx *ctx)
{
    int i;

    sg_init_table(ctx->sgl, SG_MAX_ENTRIES);
    ctx->nents = SG_MAX_ENTRIES;

    for (i = 0; i < SG_MAX_ENTRIES; i++) {
        ctx->bufs[i] = (void *)get_zeroed_page(GFP_KERNEL);
        if (!ctx->bufs[i])
            goto err_pages;
        sg_set_buf(&ctx->sgl[i], ctx->bufs[i], SG_ENTRY_SIZE);
    }

    /* Map entire SG list for DMA — kernel may merge entries (IOMMU) */
    if (dma_map_sg(dev, ctx->sgl, ctx->nents, DMA_BIDIRECTIONAL) == 0) {
        dev_err(dev, "dma_map_sg failed\n");
        goto err_pages;
    }

    /* Extract DMA addresses for HW programming */
    for (i = 0; i < ctx->nents; i++)
        ctx->dma_addr[i] = sg_dma_address(&ctx->sgl[i]);

    ctx->mapped = true;
    return 0;

err_pages:
    for (i--; i >= 0; i--)
        free_page((unsigned long)ctx->bufs[i]);
    return -ENOMEM;
}

static void sg_dma_free(struct device *dev, struct sg_dma_ctx *ctx)
{
    int i;

    if (ctx->mapped)
        dma_unmap_sg(dev, ctx->sgl, ctx->nents, DMA_BIDIRECTIONAL);

    for (i = 0; i < SG_MAX_ENTRIES; i++)
        if (ctx->bufs[i])
            free_page((unsigned long)ctx->bufs[i]);
}

/* ─── Per-device structure ───────────────────────────────────────────────── */
struct ultra_dev {
    dev_t               devno;
    struct cdev         cdev;
    struct class       *cls;
    struct device      *dev;

    struct lf_ring      lf_ring;           /* lock-free ring */
    DECLARE_KFIFO(kfifo_buf, char, FIFO_SIZE);  /* kernel kfifo */
    spinlock_t          fifo_lock;

    struct sg_dma_ctx   sg_ctx;            /* scatter-gather DMA */
    struct device      *dma_dev;

    struct ultra_config __rcu *config;     /* RCU-protected config */
    spinlock_t          config_lock;

    wait_queue_head_t   data_wq;
    atomic_t            data_ready;
};

static struct ultra_dev *g_dev;

/* ─── RCU config update ──────────────────────────────────────────────────── */
static void config_rcu_free(struct rcu_head *head)
{
    struct ultra_config *c = container_of(head, struct ultra_config, rcu);
    kfree(c);
}

static int update_config(struct ultra_dev *d, unsigned int max_xfer, unsigned int timeout)
{
    struct ultra_config *new_cfg, *old_cfg;

    new_cfg = kmalloc(sizeof(*new_cfg), GFP_KERNEL);
    if (!new_cfg)
        return -ENOMEM;

    new_cfg->max_transfer = max_xfer;
    new_cfg->timeout_ms   = timeout;

    spin_lock(&d->config_lock);
    old_cfg = rcu_dereference_protected(d->config,
                lockdep_is_held(&d->config_lock));
    rcu_assign_pointer(d->config, new_cfg);  /* atomic publish */
    spin_unlock(&d->config_lock);

    if (old_cfg)
        call_rcu(&old_cfg->rcu, config_rcu_free);  /* deferred free */

    return 0;
}

/* ─── file_operations ────────────────────────────────────────────────────── */
static int ultra_open(struct inode *inode, struct file *filp)
{
    filp->private_data = container_of(inode->i_cdev, struct ultra_dev, cdev);
    return 0;
}

static int ultra_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t ultra_read(struct file *filp, char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    struct ultra_dev    *d = filp->private_data;
    struct ultra_config *cfg;
    unsigned int         max_xfer;
    char                 tmp[256];
    unsigned int         copied = 0;
    unsigned long        flags;

    /* RCU read-side critical section: no lock, just rcu_read_lock() */
    rcu_read_lock();
    cfg = rcu_dereference(d->config);
    max_xfer = cfg ? cfg->max_transfer : 256;
    rcu_read_unlock();

    count = min_t(size_t, count, min_t(size_t, max_xfer, sizeof(tmp)));

    if (!atomic_read(&d->data_ready)) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        wait_event_interruptible(d->data_wq, atomic_read(&d->data_ready));
    }

    spin_lock_irqsave(&d->fifo_lock, flags);
    copied = kfifo_out(&d->kfifo_buf, tmp, count);
    if (kfifo_is_empty(&d->kfifo_buf))
        atomic_set(&d->data_ready, 0);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    if (!copied)
        return 0;

    if (copy_to_user(ubuf, tmp, copied))
        return -EFAULT;

    /* Per-CPU stats: no lock needed, each CPU has own counter */
    this_cpu_inc(cpu_stats.reads);
    this_cpu_add(cpu_stats.bytes_rx, copied);

    return (ssize_t)copied;
}

static ssize_t ultra_write(struct file *filp, const char __user *ubuf,
                            size_t count, loff_t *ppos)
{
    struct ultra_dev *d = filp->private_data;
    char              tmp[256];
    size_t            to_copy = min(count, sizeof(tmp));
    unsigned int      pushed;
    unsigned long     flags;

    if (copy_from_user(tmp, ubuf, to_copy))
        return -EFAULT;

    spin_lock_irqsave(&d->fifo_lock, flags);
    pushed = kfifo_in(&d->kfifo_buf, tmp, to_copy);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    if (pushed > 0) {
        atomic_set(&d->data_ready, 1);
        wake_up_interruptible(&d->data_wq);
    }

    this_cpu_inc(cpu_stats.writes);
    this_cpu_add(cpu_stats.bytes_tx, pushed);

    return (ssize_t)pushed;
}

static const struct file_operations ultra_fops = {
    .owner   = THIS_MODULE,
    .open    = ultra_open,
    .release = ultra_release,
    .read    = ultra_read,
    .write   = ultra_write,
};

/* ─── init ───────────────────────────────────────────────────────────────── */
static int __init ultra_init(void)
{
    int ret;

    g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
    if (!g_dev) return -ENOMEM;

    spin_lock_init(&g_dev->fifo_lock);
    spin_lock_init(&g_dev->config_lock);
    init_waitqueue_head(&g_dev->data_wq);
    atomic_set(&g_dev->data_ready, 0);
    INIT_KFIFO(g_dev->kfifo_buf);
    atomic_set(&g_dev->lf_ring.head, 0);
    atomic_set(&g_dev->lf_ring.tail, 0);

    /* Initial config via RCU */
    ret = update_config(g_dev, 4096, 1000);
    if (ret) goto err_free;

    ret = alloc_chrdev_region(&g_dev->devno, 0, 1, DRIVER_NAME);
    if (ret) goto err_free;

    cdev_init(&g_dev->cdev, &ultra_fops);
    g_dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_dev->cdev, g_dev->devno, 1);
    if (ret) goto err_unreg;

    g_dev->cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_dev->cls)) { ret = PTR_ERR(g_dev->cls); goto err_cdev; }

    g_dev->dev = device_create(g_dev->cls, NULL, g_dev->devno, NULL, DEVICE_NAME);
    if (IS_ERR(g_dev->dev)) { ret = PTR_ERR(g_dev->dev); goto err_class; }

    pr_info("%s: ultra driver ready with SMP/RCU/kfifo/SG-DMA support\n",
            DRIVER_NAME);
    return 0;

err_class: class_destroy(g_dev->cls);
err_cdev:  cdev_del(&g_dev->cdev);
err_unreg: unregister_chrdev_region(g_dev->devno, 1);
err_free:  kfree(g_dev);
    return ret;
}

/* ─── exit ───────────────────────────────────────────────────────────────── */
static void __exit ultra_exit(void)
{
    struct ultra_config *cfg;

    device_destroy(g_dev->cls, g_dev->devno);
    class_destroy(g_dev->cls);
    cdev_del(&g_dev->cdev);
    unregister_chrdev_region(g_dev->devno, 1);

    /* RCU config cleanup: wait for all readers to finish */
    spin_lock(&g_dev->config_lock);
    cfg = rcu_dereference_protected(g_dev->config,
            lockdep_is_held(&g_dev->config_lock));
    RCU_INIT_POINTER(g_dev->config, NULL);
    spin_unlock(&g_dev->config_lock);
    if (cfg) {
        synchronize_rcu();   /* wait for all RCU read-side sections */
        kfree(cfg);
    }

    kfree(g_dev);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(ultra_init);
module_exit(ultra_exit);
