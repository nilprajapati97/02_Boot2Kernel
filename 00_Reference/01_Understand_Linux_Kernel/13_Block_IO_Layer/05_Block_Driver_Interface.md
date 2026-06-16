# 05 — Block Driver Interface

## 1. Overview

Block drivers expose hardware to the block layer by:
1. Allocating a `gendisk`
2. Registering a `block_device_operations`
3. Setting up a `request_queue` (blk-mq)
4. Handling requests in `queue_rq()`

---

## 2. block_device_operations

```c
/* include/linux/blkdev.h */
struct block_device_operations {
    void            (*submit_bio)(struct bio *bio);    /* Fast path */
    int             (*open)(struct block_device *, fmode_t);
    void            (*release)(struct gendisk *, fmode_t);
    int             (*rw_page)(struct block_device *, sector_t,
                               struct page *, unsigned int);
    int             (*ioctl)(struct block_device *, fmode_t,
                             unsigned, unsigned long);
    int             (*compat_ioctl)(struct block_device *, fmode_t,
                                    unsigned, unsigned long);
    int             (*getgeo)(struct block_device *, struct hd_geometry *);
    int             (*set_read_only)(struct block_device *, bool);
    void            (*free_disk)(struct gendisk *);
    struct module   *owner;
};
```

---

## 3. blk-mq Driver Operations

```c
struct blk_mq_ops {
    /* Called to dispatch a request to hardware */
    blk_status_t (*queue_rq)(struct blk_mq_hw_ctx *hctx,
                             const struct blk_mq_queue_data *bd);
    /* Called after queue_rq to kick hardware if needed */
    void (*commit_rqs)(struct blk_mq_hw_ctx *hctx);
    /* Timeout handler */
    enum blk_eh_timer_return (*timeout)(struct request *req);
    /* Poll for completions (for NVMe polling) */
    int (*poll)(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *);
    /* Setup a hardware queue (per IRQ) */
    int (*init_hctx)(struct blk_mq_hw_ctx *hctx, void *driver_data, unsigned int index);
};
```

---

## 4. Minimal RAM Disk Driver Skeleton

```c
#include <linux/blkdev.h>
#include <linux/blk-mq.h>

#define RAMDISK_SECTORS   65536  /* 32 MiB */
static u8 *ramdisk_data;

static blk_status_t ramdisk_queue_rq(struct blk_mq_hw_ctx *hctx,
                                      const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct bio_vec bvec;
    struct req_iterator iter;
    sector_t sector = blk_rq_pos(rq);

    blk_mq_start_request(rq);

    rq_for_each_segment(bvec, rq, iter) {
        u8  *buf  = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
        u64  off  = sector << SECTOR_SHIFT;
        
        if (rq_data_dir(rq) == WRITE)
            memcpy(ramdisk_data + off, buf, bvec.bv_len);
        else
            memcpy(buf, ramdisk_data + off, bvec.bv_len);
        
        kunmap_local(buf);
        sector += bvec.bv_len >> SECTOR_SHIFT;
    }

    blk_mq_end_request(rq, BLK_STS_OK);
    return BLK_STS_OK;
}

static struct blk_mq_ops ramdisk_mq_ops = {
    .queue_rq = ramdisk_queue_rq,
};

static struct block_device_operations ramdisk_bdev_ops = {
    .owner = THIS_MODULE,
};

static int __init ramdisk_init(void)
{
    struct blk_mq_tag_set tag_set = {
        .ops      = &ramdisk_mq_ops,
        .nr_hw_queues = 1,
        .queue_depth  = 64,
        .numa_node    = NUMA_NO_NODE,
        .flags        = BLK_MQ_F_SHOULD_MERGE,
    };
    struct gendisk *disk;

    ramdisk_data = vzalloc(RAMDISK_SECTORS << SECTOR_SHIFT);

    disk = blk_mq_alloc_disk(&tag_set, NULL);
    disk->major       = 0;  /* auto-assign */
    disk->first_minor = 0;
    disk->minors      = 1;
    disk->fops        = &ramdisk_bdev_ops;
    set_capacity(disk, RAMDISK_SECTORS);
    snprintf(disk->disk_name, DISK_NAME_LEN, "ramdisk0");

    add_disk(disk);
    return 0;
}
```

---

## 5. Request Status Codes

| Code | Meaning |
|------|---------|
| `BLK_STS_OK` | Success |
| `BLK_STS_IOERR` | I/O error |
| `BLK_STS_TIMEOUT` | Timed out |
| `BLK_STS_RESOURCE` | No resources, retry |
| `BLK_STS_AGAIN` | Requeue request |
| `BLK_STS_NOTSUPP` | Not supported |

---

## 6. Source Files

| File | Description |
|------|-------------|
| `block/blk-mq.c` | blk-mq framework |
| `drivers/block/brd.c` | RAM disk (reference driver) |
| `drivers/nvme/host/` | NVMe driver |
| `include/linux/blkdev.h` | All block driver structs |

---

## 7. Related Topics
- [04_Request_Queue.md](./04_Request_Queue.md)
- [02_Bio_Structure.md](./02_Bio_Structure.md)
- [../16_Devices_And_Modules/01_Device_Model.md](../16_Devices_And_Modules/01_Device_Model.md)
