// =============================================================================
// platform_dma_driver.c — Level 04: Platform Driver + Device Tree + DMA
// =============================================================================
// Production SoC pattern (Qualcomm style):
//   ✅ platform_driver — probed by kernel from Device Tree
//   ✅ of_match_table — DT compatible string matching
//   ✅ devm_* resource helpers — automatic cleanup on driver remove
//   ✅ DMA coherent buffer — zero-copy kernel↔device transfers
//   ✅ IRQ from DT — devm_request_irq
//   ✅ Char device on top of platform driver
//   ✅ MMIO register access via ioread32/iowrite32
// =============================================================================

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Kumar");
MODULE_DESCRIPTION("Level 04 — Platform Driver + DMA + Device Tree");
MODULE_VERSION("4.0");

/* ─── Configuration ─────────────────────────────────────────────────────── */
#define DRIVER_NAME     "plat_dma_dev"
#define DEVICE_NAME     "MyAnilDev4"
#define CLASS_NAME      "plat_class"
#define DMA_BUF_SIZE    PAGE_SIZE      /* 4096 bytes — page-aligned for DMA */

/* ─── Simulated HW register offsets ─────────────────────────────────────── */
#define REG_CTRL        0x00
#define REG_STATUS      0x04
#define REG_DMA_ADDR_LO 0x08
#define REG_DMA_ADDR_HI 0x0C
#define REG_DMA_LEN     0x10

#define CTRL_DMA_START  BIT(0)
#define CTRL_IRQ_EN     BIT(1)
#define STATUS_DONE     BIT(0)
#define STATUS_ERROR    BIT(1)

/* ─── Per-device private structure ──────────────────────────────────────── */
struct plat_dma_priv {
    /* Platform device */
    struct platform_device *pdev;

    /* MMIO registers */
    void __iomem           *base;

    /* DMA coherent buffer */
    void                   *dma_vaddr;     /* kernel virtual address */
    dma_addr_t              dma_paddr;     /* physical address given to HW */
    size_t                  dma_len;       /* bytes transferred by DMA */

    /* Char device */
    dev_t                   devno;
    struct cdev             cdev;
    struct class           *cls;
    struct device          *chrdev;

    /* Synchronization */
    struct mutex            lock;
    wait_queue_head_t       dma_wq;
    bool                    dma_done;

    /* IRQ */
    int                     irq;
};

/* ─── MMIO helpers ───────────────────────────────────────────────────────── */
static inline void hw_write(struct plat_dma_priv *p, u32 reg, u32 val)
{
    iowrite32(val, p->base + reg);
}

static inline u32 hw_read(struct plat_dma_priv *p, u32 reg)
{
    return ioread32(p->base + reg);
}

/* ─── IRQ handler — DMA completion interrupt ─────────────────────────────── */
static irqreturn_t plat_dma_irq(int irq, void *data)
{
    struct plat_dma_priv *p = data;
    u32 status;

    status = hw_read(p, REG_STATUS);
    if (!(status & STATUS_DONE))
        return IRQ_NONE;   /* not our interrupt */

    /* Clear interrupt by writing back */
    hw_write(p, REG_STATUS, STATUS_DONE);

    p->dma_done = true;
    wake_up_interruptible(&p->dma_wq);

    dev_dbg(&p->pdev->dev, "DMA complete, status=0x%x\n", status);
    return IRQ_HANDLED;
}

/* ─── DMA transfer: submit and wait ─────────────────────────────────────── */
static int plat_dma_transfer(struct plat_dma_priv *p, size_t len)
{
    u64 addr = (u64)p->dma_paddr;

    if (len > DMA_BUF_SIZE)
        return -EINVAL;

    p->dma_done = false;
    p->dma_len  = len;

    /*
     * Program DMA engine registers with physical address.
     * The CPU cannot directly address physical addresses —
     * only the DMA engine/IOMMU uses paddr.
     */
    hw_write(p, REG_DMA_ADDR_LO, (u32)(addr & 0xFFFFFFFF));
    hw_write(p, REG_DMA_ADDR_HI, (u32)(addr >> 32));
    hw_write(p, REG_DMA_LEN,     (u32)len);
    hw_write(p, REG_CTRL,        CTRL_DMA_START | CTRL_IRQ_EN);

    /* Wait for DMA completion IRQ */
    if (wait_event_interruptible_timeout(p->dma_wq, p->dma_done, HZ) <= 0) {
        dev_err(&p->pdev->dev, "DMA timeout!\n");
        return -ETIMEDOUT;
    }

    return 0;
}

/* ─── file_operations ────────────────────────────────────────────────────── */
static int plat_open(struct inode *inode, struct file *filp)
{
    struct plat_dma_priv *p =
        container_of(inode->i_cdev, struct plat_dma_priv, cdev);
    filp->private_data = p;
    return 0;
}

static int plat_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * read(): trigger DMA from HW → kernel DMA buffer → copy to user
 * This is the zero-copy intent: data arrives via DMA, no CPU memcpy from HW
 */
static ssize_t plat_read(struct file *filp, char __user *ubuf,
                          size_t count, loff_t *ppos)
{
    struct plat_dma_priv *p = filp->private_data;
    ssize_t ret;

    mutex_lock(&p->lock);

    /* Trigger DMA read from HW into our coherent buffer */
    ret = plat_dma_transfer(p, min(count, DMA_BUF_SIZE));
    if (ret < 0) {
        mutex_unlock(&p->lock);
        return ret;
    }

    /* DMA coherent: no cache flush needed — CPU sees data immediately */
    if (copy_to_user(ubuf, p->dma_vaddr, p->dma_len)) {
        mutex_unlock(&p->lock);
        return -EFAULT;
    }

    mutex_unlock(&p->lock);
    return (ssize_t)p->dma_len;
}

/*
 * write(): copy from user → kernel DMA buffer → trigger DMA to HW
 */
static ssize_t plat_write(struct file *filp, const char __user *ubuf,
                           size_t count, loff_t *ppos)
{
    struct plat_dma_priv *p = filp->private_data;
    size_t to_copy = min(count, DMA_BUF_SIZE);
    ssize_t ret;

    mutex_lock(&p->lock);

    if (copy_from_user(p->dma_vaddr, ubuf, to_copy)) {
        mutex_unlock(&p->lock);
        return -EFAULT;
    }

    ret = plat_dma_transfer(p, to_copy);
    mutex_unlock(&p->lock);

    return ret < 0 ? ret : (ssize_t)to_copy;
}

static const struct file_operations plat_fops = {
    .owner   = THIS_MODULE,
    .open    = plat_open,
    .release = plat_release,
    .read    = plat_read,
    .write   = plat_write,
};

/* ─── platform_driver: probe ─────────────────────────────────────────────── */
/*
 * probe() is called by the kernel when DT node matches compatible string.
 * All resource acquisition uses devm_* → auto-freed on driver removal.
 */
static int plat_dma_probe(struct platform_device *pdev)
{
    struct plat_dma_priv *p;
    struct resource       *res;
    int                    ret;

    dev_info(&pdev->dev, "probe() called\n");

    /* Allocate private data — devm_: auto-freed when pdev is removed */
    p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
    if (!p)
        return -ENOMEM;

    p->pdev = pdev;
    platform_set_drvdata(pdev, p);

    mutex_init(&p->lock);
    init_waitqueue_head(&p->dma_wq);

    /* Map MMIO registers from DT "reg" property */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    p->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(p->base))
        return PTR_ERR(p->base);

    /* Allocate DMA coherent buffer */
    p->dma_vaddr = dma_alloc_coherent(&pdev->dev, DMA_BUF_SIZE,
                                       &p->dma_paddr, GFP_KERNEL);
    if (!p->dma_vaddr)
        return -ENOMEM;

    dev_info(&pdev->dev, "DMA buf: vaddr=%p paddr=%pad\n",
             p->dma_vaddr, &p->dma_paddr);

    /* Get IRQ from DT "interrupts" property */
    p->irq = platform_get_irq(pdev, 0);
    if (p->irq < 0)
        goto err_dma;

    ret = devm_request_irq(&pdev->dev, p->irq, plat_dma_irq,
                            IRQF_SHARED, DRIVER_NAME, p);
    if (ret)
        goto err_dma;

    /* Register char device on top of platform device */
    ret = alloc_chrdev_region(&p->devno, 0, 1, DRIVER_NAME);
    if (ret) goto err_dma;

    cdev_init(&p->cdev, &plat_fops);
    p->cdev.owner = THIS_MODULE;
    ret = cdev_add(&p->cdev, p->devno, 1);
    if (ret) goto err_unreg;

    p->cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(p->cls)) { ret = PTR_ERR(p->cls); goto err_cdev; }

    p->chrdev = device_create(p->cls, &pdev->dev,
                              p->devno, NULL, DEVICE_NAME);
    if (IS_ERR(p->chrdev)) { ret = PTR_ERR(p->chrdev); goto err_class; }

    dev_info(&pdev->dev, "/dev/%s ready\n", DEVICE_NAME);
    return 0;

err_class: class_destroy(p->cls);
err_cdev:  cdev_del(&p->cdev);
err_unreg: unregister_chrdev_region(p->devno, 1);
err_dma:   dma_free_coherent(&pdev->dev, DMA_BUF_SIZE,
                              p->dma_vaddr, p->dma_paddr);
    return ret;
}

/* ─── platform_driver: remove ────────────────────────────────────────────── */
static int plat_dma_remove(struct platform_device *pdev)
{
    struct plat_dma_priv *p = platform_get_drvdata(pdev);

    device_destroy(p->cls, p->devno);
    class_destroy(p->cls);
    cdev_del(&p->cdev);
    unregister_chrdev_region(p->devno, 1);
    dma_free_coherent(&pdev->dev, DMA_BUF_SIZE, p->dma_vaddr, p->dma_paddr);
    /* devm resources (ioremap, irq, kzalloc) freed automatically */

    dev_info(&pdev->dev, "removed\n");
    return 0;
}

/* ─── Device Tree matching table ─────────────────────────────────────────── */
static const struct of_device_id plat_dma_of_match[] = {
    { .compatible = "myvendor,anil-dma-device" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, plat_dma_of_match);

/* ─── platform_driver registration ──────────────────────────────────────── */
static struct platform_driver plat_dma_driver = {
    .probe  = plat_dma_probe,
    .remove = plat_dma_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = plat_dma_of_match,
    },
};

module_platform_driver(plat_dma_driver);
/* Equivalent to:
 *   module_init → platform_driver_register(&plat_dma_driver)
 *   module_exit → platform_driver_unregister(&plat_dma_driver)
 */
