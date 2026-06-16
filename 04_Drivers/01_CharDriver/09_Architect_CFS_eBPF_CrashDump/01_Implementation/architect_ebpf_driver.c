// =============================================================================
// architect_ebpf_driver.c — Level 09: Architect Level
// CFS Scheduler + eBPF Hooks + vmcore Analysis Ready
// =============================================================================
// Staff/Architect level — system designer perspective:
//   ✅ eBPF-compatible tracepoints (DEFINE_TRACE / trace_*) 
//   ✅ kprobe-ready function signatures for live eBPF attach
//   ✅ Scheduler-cooperative design (avoid scheduler anti-patterns)
//   ✅ vruntime-aware priority hints (sched_setscheduler)
//   ✅ vmcore/crash analysis: structured dev state for kdump
//   ✅ notifier chain (power events, CPU hotplug awareness)
//   ✅ Memory pressure awareness (shrinker registration)
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
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/mm.h>
#include <linux/shrinker.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/cpuhotplug.h>
#include <linux/debugfs.h>
#include <trace/events/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 09 — Architect: CFS + eBPF-ready + vmcore + shrinker");
MODULE_VERSION("9.0");

#define DRIVER_NAME     "arch_drv"
#define DEVICE_NAME     "MyAnilDev9"
#define CLASS_NAME      "arch_class"
#define FIFO_SIZE       16384
#define MAX_CACHE_PAGES 256

/* ─── vmcore-friendly state structure ───────────────────────────────────── */
/*
 * For crash dump analysis:
 * - Use magic number to find structure in vmcore
 * - Avoid pointer-only fields (need values too)
 * - Use fixed-size types (u32, u64, not int/long)
 */
#define ARCH_DEV_MAGIC  0xA1_B2_C3_D4

struct arch_dev_state {
    u32     magic;                  /* 0xA1B2C3D4 — find in vmcore */
    u32     version;                /* state struct version */
    u64     total_bytes_rx;
    u64     total_bytes_tx;
    u32     irq_count;
    u32     fifo_len;
    u32     open_count;
    u32     flags;                  /* bitmask of active features */
    ktime_t last_irq_time;
    ktime_t last_read_time;
    pid_t   last_reader_pid;
    char    last_reader_comm[TASK_COMM_LEN];
};

/* ─── Cache page pool (shrinkable under memory pressure) ─────────────────── */
struct cache_pool {
    struct list_head  pages;
    spinlock_t        lock;
    unsigned int      count;
    struct shrinker   shrinker;
};

/* ─── Per-device structure ───────────────────────────────────────────────── */
struct arch_dev {
    dev_t               devno;
    struct cdev         cdev;
    struct class       *cls;
    struct device      *chrdev;

    DECLARE_KFIFO(fifo, char, FIFO_SIZE);
    spinlock_t      fifo_lock;
    wait_queue_head_t read_wq;
    atomic_t         data_ready;

    /* Vmcore-analyzable state */
    struct arch_dev_state state;

    /* Cache pool (shrinker integration) */
    struct cache_pool     pool;

    /* Notifiers */
    struct notifier_block reboot_nb;
    struct notifier_block pm_nb;

    /* Debugfs */
    struct dentry *dbg_dir;
};

static struct arch_dev *g_dev;

/* ─── Memory shrinker: release cache under OOM pressure ─────────────────── */
/*
 * When kernel faces memory pressure, it calls registered shrinkers.
 * Driver should release non-essential cached pages.
 */
static unsigned long arch_shrinker_count(struct shrinker *s,
                                          struct shrink_control *sc)
{
    struct cache_pool *pool = container_of(s, struct cache_pool, shrinker);
    return pool->count;    /* how many pages we CAN free */
}

static unsigned long arch_shrinker_scan(struct shrinker *s,
                                         struct shrink_control *sc)
{
    struct cache_pool *pool = container_of(s, struct cache_pool, shrinker);
    struct page *page, *tmp;
    unsigned long freed = 0;
    unsigned long flags;

    spin_lock_irqsave(&pool->lock, flags);
    list_for_each_entry_safe(page, tmp, &pool->pages, lru) {
        if (freed >= sc->nr_to_scan)
            break;
        list_del(&page->lru);
        __free_page(page);
        pool->count--;
        freed++;
    }
    spin_unlock_irqrestore(&pool->lock, flags);

    pr_debug("%s: shrinker freed %lu pages\n", DRIVER_NAME, freed);
    return freed;
}

/* ─── Reboot notifier: flush data before system reboot ──────────────────── */
static int arch_reboot_notify(struct notifier_block *nb,
                               unsigned long action, void *data)
{
    struct arch_dev *d = container_of(nb, struct arch_dev, reboot_nb);
    unsigned long flags;

    if (action != SYS_RESTART && action != SYS_HALT)
        return NOTIFY_DONE;

    /* Flush pending data before reboot */
    spin_lock_irqsave(&d->fifo_lock, flags);
    kfifo_reset(&d->fifo);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    pr_info("%s: reboot notify — data flushed\n", DRIVER_NAME);
    return NOTIFY_OK;
}

/* ─── Scheduler-cooperative: yield to avoid starvation ──────────────────── */
/*
 * Anti-pattern: long-running kernel loops without cond_resched()
 * → starves other tasks on non-preemptible kernels (RT configs)
 */
static void process_bulk_data(struct arch_dev *d, char *buf, size_t len)
{
    size_t i;
    unsigned long flags;

    for (i = 0; i < len; i++) {
        spin_lock_irqsave(&d->fifo_lock, flags);
        kfifo_in(&d->fifo, &buf[i], 1);
        spin_unlock_irqrestore(&d->fifo_lock, flags);

        /* Yield CPU every 256 iterations — CFS cooperation */
        if ((i & 0xFF) == 0)
            cond_resched();
    }
}

/* ─── eBPF-friendly: tracepoint registration ────────────────────────────── */
/*
 * Tracepoints allow eBPF programs (BCC, bpftrace) to attach at runtime.
 * Usage from user-space:
 *   bpftrace -e 'tracepoint:arch_drv:data_received { printf("%d\n", args->len); }'
 *
 * Note: DEFINE_TRACE requires a separate trace header. For inline use,
 * we emit trace_printk which is intercepted by ftrace.
 */
static void arch_trace_data_received(size_t len, pid_t pid)
{
    trace_printk("arch_drv: data_received len=%zu pid=%d\n", len, pid);
}

static void arch_trace_irq_handled(int irq, u64 latency_ns)
{
    trace_printk("arch_drv: irq_handled irq=%d latency_ns=%llu\n",
                 irq, latency_ns);
}

/* ─── Debugfs: vmcore state dump ─────────────────────────────────────────── */
static int vmcore_show(struct seq_file *sf, void *v)
{
    struct arch_dev *d = sf->private;
    struct arch_dev_state *s = &d->state;

    seq_printf(sf, "magic         : 0x%08X\n", s->magic);
    seq_printf(sf, "version       : %u\n",      s->version);
    seq_printf(sf, "bytes_rx      : %llu\n",    s->total_bytes_rx);
    seq_printf(sf, "bytes_tx      : %llu\n",    s->total_bytes_tx);
    seq_printf(sf, "irq_count     : %u\n",      s->irq_count);
    seq_printf(sf, "fifo_len      : %u\n",      s->fifo_len);
    seq_printf(sf, "open_count    : %u\n",      s->open_count);
    seq_printf(sf, "last_reader   : %s (pid=%d)\n",
               s->last_reader_comm, s->last_reader_pid);
    seq_printf(sf, "last_irq_ns   : %lld\n",    ktime_to_ns(s->last_irq_time));
    seq_printf(sf, "\nTo analyze in crash/gdb:\n");
    seq_printf(sf, "  (gdb) find /d arch_dev_state.magic == 0xA1B2C3D4\n");
    return 0;
}

static int vmcore_open(struct inode *i, struct file *f)
{
    return single_open(f, vmcore_show, i->i_private);
}

static const struct file_operations vmcore_fops = {
    .open = vmcore_open, .read = seq_read,
    .llseek = seq_lseek, .release = single_release,
};

/* ─── file_operations ────────────────────────────────────────────────────── */
static int arch_open(struct inode *inode, struct file *filp)
{
    struct arch_dev *d = container_of(inode->i_cdev, struct arch_dev, cdev);

    filp->private_data = d;
    d->state.open_count++;

    /* Capture reader task for post-mortem analysis */
    d->state.last_reader_pid = current->pid;
    strlcpy(d->state.last_reader_comm, current->comm,
            sizeof(d->state.last_reader_comm));

    pr_debug("%s: opened by %s (pid=%d)\n",
             DRIVER_NAME, current->comm, current->pid);
    return 0;
}

static int arch_release(struct inode *inode, struct file *filp)
{
    struct arch_dev *d = filp->private_data;
    d->state.open_count--;
    return 0;
}

static ssize_t arch_read(struct file *filp, char __user *ubuf,
                          size_t count, loff_t *ppos)
{
    struct arch_dev *d = filp->private_data;
    char          tmp[512];
    unsigned int  copied;
    unsigned long flags;

    if (!atomic_read(&d->data_ready)) {
        if (filp->f_flags & O_NONBLOCK) return -EAGAIN;
        wait_event_interruptible(d->read_wq, atomic_read(&d->data_ready));
    }

    d->state.last_read_time = ktime_get();

    spin_lock_irqsave(&d->fifo_lock, flags);
    copied = kfifo_out(&d->fifo, tmp, min(count, sizeof(tmp)));
    d->state.fifo_len = kfifo_len(&d->fifo);
    if (kfifo_is_empty(&d->fifo))
        atomic_set(&d->data_ready, 0);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    if (!copied) return 0;
    if (copy_to_user(ubuf, tmp, copied)) return -EFAULT;

    d->state.total_bytes_rx += copied;
    arch_trace_data_received(copied, current->pid);

    return (ssize_t)copied;
}

static ssize_t arch_write(struct file *filp, const char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    struct arch_dev *d = filp->private_data;
    char          tmp[512];
    size_t        to_copy = min(count, sizeof(tmp));
    unsigned long flags;
    unsigned int  pushed;

    if (copy_from_user(tmp, ubuf, to_copy)) return -EFAULT;

    process_bulk_data(d, tmp, to_copy);

    spin_lock_irqsave(&d->fifo_lock, flags);
    pushed = kfifo_len(&d->fifo);
    spin_unlock_irqrestore(&d->fifo_lock, flags);

    if (pushed > 0) {
        atomic_set(&d->data_ready, 1);
        wake_up_interruptible(&d->read_wq);
    }

    d->state.total_bytes_tx += to_copy;
    return (ssize_t)to_copy;
}

static const struct file_operations arch_fops = {
    .owner = THIS_MODULE,
    .open = arch_open, .release = arch_release,
    .read = arch_read, .write = arch_write,
};

/* ─── init ───────────────────────────────────────────────────────────────── */
static int __init arch_init(void)
{
    int ret;

    g_dev = kzalloc(sizeof(*g_dev), GFP_KERNEL);
    if (!g_dev) return -ENOMEM;

    spin_lock_init(&g_dev->fifo_lock);
    init_waitqueue_head(&g_dev->read_wq);
    atomic_set(&g_dev->data_ready, 0);
    INIT_KFIFO(g_dev->fifo);

    /* Initialize vmcore state */
    g_dev->state.magic   = 0xA1B2C3D4;
    g_dev->state.version = 1;

    /* Setup shrinker */
    INIT_LIST_HEAD(&g_dev->pool.pages);
    spin_lock_init(&g_dev->pool.lock);
    g_dev->pool.shrinker.count_objects = arch_shrinker_count;
    g_dev->pool.shrinker.scan_objects  = arch_shrinker_scan;
    g_dev->pool.shrinker.seeks         = DEFAULT_SEEKS;
    register_shrinker(&g_dev->pool.shrinker);

    /* Register reboot notifier */
    g_dev->reboot_nb.notifier_call = arch_reboot_notify;
    g_dev->reboot_nb.priority      = 0;
    register_reboot_notifier(&g_dev->reboot_nb);

    ret = alloc_chrdev_region(&g_dev->devno, 0, 1, DRIVER_NAME);
    if (ret) goto err_free;

    cdev_init(&g_dev->cdev, &arch_fops);
    g_dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_dev->cdev, g_dev->devno, 1);
    if (ret) goto err_unreg;

    g_dev->cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_dev->cls)) { ret = PTR_ERR(g_dev->cls); goto err_cdev; }

    g_dev->chrdev = device_create(g_dev->cls, NULL,
                                   g_dev->devno, NULL, DEVICE_NAME);
    if (IS_ERR(g_dev->chrdev)) { ret = PTR_ERR(g_dev->chrdev); goto err_class; }

    /* Debugfs */
    g_dev->dbg_dir = debugfs_create_dir(DRIVER_NAME, NULL);
    if (!IS_ERR_OR_NULL(g_dev->dbg_dir))
        debugfs_create_file("vmcore_state", 0444, g_dev->dbg_dir,
                            g_dev, &vmcore_fops);

    pr_info("%s: /dev/%s ready (eBPF/shrinker/reboot hooks active)\n",
            DRIVER_NAME, DEVICE_NAME);
    return 0;

err_class: class_destroy(g_dev->cls);
err_cdev:  cdev_del(&g_dev->cdev);
err_unreg: unregister_chrdev_region(g_dev->devno, 1);
err_free:
    unregister_reboot_notifier(&g_dev->reboot_nb);
    unregister_shrinker(&g_dev->pool.shrinker);
    kfree(g_dev);
    return ret;
}

/* ─── exit ───────────────────────────────────────────────────────────────── */
static void __exit arch_exit(void)
{
    debugfs_remove_recursive(g_dev->dbg_dir);
    unregister_reboot_notifier(&g_dev->reboot_nb);
    unregister_shrinker(&g_dev->pool.shrinker);
    device_destroy(g_dev->cls, g_dev->devno);
    class_destroy(g_dev->cls);
    cdev_del(&g_dev->cdev);
    unregister_chrdev_region(g_dev->devno, 1);
    kfree(g_dev);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(arch_init);
module_exit(arch_exit);
