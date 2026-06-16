# Day 22 — virtio-blk Driver (MMIO Transport)

> **Goal**: Detect a virtio-mmio device, negotiate features, set up a single virtqueue (descriptor table + avail + used rings), and implement synchronous `block_read(sector, n, buf)` / `block_write(sector, n, buf)`. This block device backs the ext2 filesystem on Day 23.
>
> **Why today**: QEMU `virt` exposes virtio devices on MMIO; we need a real block backend before we can implement a real on-disk FS.

---

## 1. Background

### 1.1 QEMU virt MMIO virtio region
By default, QEMU `virt` allocates virtio-mmio slots starting at `0x0A000000`, each 0x200 bytes, 32 slots. The FDT enumerates them under `/virtio_mmio@…`. Each slot is the same register layout (spec v1.1):

| Offset | Reg | RW |
|---|---|---|
| 0x000 | MagicValue (`0x74726976` = "virt") | R |
| 0x004 | Version (1 = legacy, 2 = modern) | R |
| 0x008 | DeviceID (2 = block) | R |
| 0x00C | VendorID | R |
| 0x010 | DeviceFeatures (windowed) | R |
| 0x014 | DeviceFeaturesSel | W |
| 0x020 | DriverFeatures | W |
| 0x024 | DriverFeaturesSel | W |
| 0x030 | QueueSel | W |
| 0x034 | QueueNumMax | R |
| 0x038 | QueueNum | W |
| 0x044 | QueueReady | RW |
| 0x050 | QueueNotify | W |
| 0x060 | InterruptStatus | R |
| 0x064 | InterruptACK | W |
| 0x070 | Status | RW |
| 0x080 | QueueDescLow / 0x084 High | W |
| 0x090 | QueueAvailLow / 0x094 High | W |
| 0x0A0 | QueueUsedLow / 0x0A4 High | W |
| 0x0FC | ConfigGeneration | R |
| 0x100+ | Device-specific config (capacity etc.) | R |

### 1.2 Status bits
```
ACK(1) → DRIVER(2) → FEATURES_OK(8) → DRIVER_OK(4)
```
Write each bit cumulatively. If `FEATURES_OK` does not stick after write, you negotiated illegal features.

### 1.3 Virtqueue (split, modern v2)
Three structures, page-aligned (we'll round to 4 KiB):
```c
struct vring_desc {
    u64 addr;   /* physical */
    u32 len;
    u16 flags;  /* NEXT=1, WRITE=2, INDIRECT=4 */
    u16 next;
};

struct vring_avail {
    u16 flags;
    u16 idx;
    u16 ring[QUEUE_SIZE];
};

struct vring_used_elem { u32 id; u32 len; };

struct vring_used {
    u16 flags;
    u16 idx;
    struct vring_used_elem ring[QUEUE_SIZE];
};
```
`QUEUE_SIZE` = 16 is plenty.

### 1.4 virtio-blk request format
```c
struct virtio_blk_req_hdr {
    u32 type;       /* IN=0 read, OUT=1 write, FLUSH=4 */
    u32 reserved;
    u64 sector;     /* 512-byte units */
};
```
A request uses **3 descriptors** chained with `NEXT`:
1. `hdr` (read by device)
2. `data` (read by device for write, write by device for read)
3. `status` byte (write by device, 0 = OK)

---

## 2. Design

### 2.1 Files
```
include/drivers/virtio_blk.h
drivers/virtio/virtio_mmio.c       probe loop
drivers/virtio/virtio_blk.c        block driver
```

### 2.2 Driver state
```c
struct virtq {
    u16 size;
    u16 free_head;
    u16 last_used;
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    phys_addr_t desc_pa, avail_pa, used_pa;
};

struct vblk {
    volatile u32 *regs;     /* MMIO base */
    struct virtq vq;
    u64 capacity_sectors;
    spinlock_t lock;
    wait_queue_head_t wait;
    int in_flight;
};
static struct vblk vblk0;
```

---

## 3. Implementation

### 3.1 Probe
```c
#define V_MMIO(base, off) ((volatile u32*)((u8*)(base) + (off)))

int virtio_mmio_probe(u64 base_pa)
{
    void *base = (void*)(base_pa + 0xffff000000000000UL);
    if (*V_MMIO(base, 0x000) != 0x74726976) return -1;
    u32 ver = *V_MMIO(base, 0x004);
    u32 dev = *V_MMIO(base, 0x008);
    if (dev == 0) return -1;       /* empty slot */
    if (dev == 2 && ver >= 2)
        return virtio_blk_init(base);
    return -1;
}

void virtio_mmio_scan(void)
{
    /* From FDT we know there are N slots; bruteforce check */
    for (int i = 0; i < 32; i++)
        virtio_mmio_probe(0x0A000000 + i * 0x200);
}
```

### 3.2 Feature negotiation
```c
static int vblk_negotiate(volatile u32 *r)
{
    *V_MMIO(r, 0x070) = 0;                 /* RESET */
    *V_MMIO(r, 0x070) = 1;                 /* ACK */
    *V_MMIO(r, 0x070) = 1 | 2;             /* DRIVER */

    /* Read device features 0..31 */
    *V_MMIO(r, 0x014) = 0;
    u32 df = *V_MMIO(r, 0x010);
    u32 want = df & (1u << 5);             /* VIRTIO_BLK_F_BLK_SIZE optional; just accept VERSION_1 */
    /* Always set VIRTIO_F_VERSION_1 in feature word 1 */
    *V_MMIO(r, 0x024) = 1;
    *V_MMIO(r, 0x020) = 1u << 0;           /* VERSION_1 bit32 (i.e. bit 0 of word 1) */
    *V_MMIO(r, 0x024) = 0;
    *V_MMIO(r, 0x020) = want;

    *V_MMIO(r, 0x070) = 1 | 2 | 8;         /* FEATURES_OK */
    if (!(*V_MMIO(r, 0x070) & 8)) return -1;
    return 0;
}
```

### 3.3 Queue setup
```c
static int vblk_setup_queue(struct vblk *v)
{
    *V_MMIO(v->regs, 0x030) = 0;            /* QueueSel=0 */
    u32 maxn = *V_MMIO(v->regs, 0x034);
    u16 n = maxn < 16 ? (u16)maxn : 16;
    *V_MMIO(v->regs, 0x038) = n;

    /* Allocate three contiguous pages */
    phys_addr_t pd = alloc_pages(0);
    phys_addr_t pa = alloc_pages(0);
    phys_addr_t pu = alloc_pages(0);
    v->vq.size = n;
    v->vq.desc  = (void*)(pd + 0xffff000000000000UL);
    v->vq.avail = (void*)(pa + 0xffff000000000000UL);
    v->vq.used  = (void*)(pu + 0xffff000000000000UL);
    v->vq.desc_pa = pd; v->vq.avail_pa = pa; v->vq.used_pa = pu;
    memset(v->vq.desc, 0, PAGE_SIZE);
    memset(v->vq.avail, 0, PAGE_SIZE);
    memset(v->vq.used,  0, PAGE_SIZE);

    /* Build descriptor free list */
    for (u16 i = 0; i < n-1; i++) v->vq.desc[i].next = i + 1;
    v->vq.free_head = 0;
    v->vq.last_used = 0;

    *V_MMIO(v->regs, 0x080) = (u32)pd;
    *V_MMIO(v->regs, 0x084) = (u32)(pd >> 32);
    *V_MMIO(v->regs, 0x090) = (u32)pa;
    *V_MMIO(v->regs, 0x094) = (u32)(pa >> 32);
    *V_MMIO(v->regs, 0x0A0) = (u32)pu;
    *V_MMIO(v->regs, 0x0A4) = (u32)(pu >> 32);
    *V_MMIO(v->regs, 0x044) = 1;            /* QueueReady */
    return 0;
}
```

### 3.4 Submit + wait
```c
static int vq_alloc_desc(struct virtq *q) {
    if (q->free_head == 0xffff) return -1;
    int i = q->free_head;
    q->free_head = q->desc[i].next;
    return i;
}
static void vq_free_chain(struct virtq *q, int head) {
    int i = head;
    while (q->desc[i].flags & 1 /*NEXT*/) {
        int n = q->desc[i].next;
        q->desc[i].next = q->free_head;
        q->free_head = i;
        i = n;
    }
    q->desc[i].next = q->free_head;
    q->free_head = i;
}

int virtio_blk_rw(struct vblk *v, u64 sector, void *buf, u32 sectors, int write)
{
    struct virtio_blk_req_hdr hdr = {
        .type = write ? 1 : 0, .sector = sector
    };
    u8 status = 0xff;
    phys_addr_t hdr_pa  = virt_to_phys(&hdr);
    phys_addr_t buf_pa  = virt_to_phys(buf);
    phys_addr_t st_pa   = virt_to_phys(&status);

    spin_lock_irqsave(&v->lock);
    int d0 = vq_alloc_desc(&v->vq);
    int d1 = vq_alloc_desc(&v->vq);
    int d2 = vq_alloc_desc(&v->vq);
    v->vq.desc[d0] = (struct vring_desc){.addr=hdr_pa,.len=sizeof hdr,.flags=1,.next=d1};
    v->vq.desc[d1] = (struct vring_desc){
        .addr=buf_pa, .len=sectors*512,
        .flags = 1 | (write ? 0 : 2 /*WRITE-by-device*/), .next=d2 };
    v->vq.desc[d2] = (struct vring_desc){.addr=st_pa,.len=1,.flags=2,.next=0};

    u16 ai = v->vq.avail->idx % v->vq.size;
    v->vq.avail->ring[ai] = d0;
    dmb_ish();
    v->vq.avail->idx++;
    dmb_ish();

    *V_MMIO(v->regs, 0x050) = 0;            /* QueueNotify, qidx=0 */

    /* Wait for used.idx to advance (interrupt-driven; falls back to spin) */
    wait_event(v->wait, v->vq.used->idx != v->vq.last_used);
    v->vq.last_used = v->vq.used->idx;
    vq_free_chain(&v->vq, d0);
    spin_unlock_irqrestore(&v->lock);

    return status == 0 ? 0 : -5;            /* -EIO */
}
```

### 3.5 IRQ handler
```c
void virtio_blk_irq(int irqno)
{
    u32 st = *V_MMIO(vblk0.regs, 0x060);
    *V_MMIO(vblk0.regs, 0x064) = st;
    wake_up(&vblk0.wait);
}
```
Wire to GIC SPI listed in the FDT for that virtio slot (parse `/virtio_mmio@…/interrupts`).

### 3.6 Block layer wrapper
```c
int block_read (u64 sector, u32 nsec, void *buf) { return virtio_blk_rw(&vblk0, sector, buf, nsec, 0); }
int block_write(u64 sector, u32 nsec, void *buf) { return virtio_blk_rw(&vblk0, sector, buf, nsec, 1); }
```

---

## 4. QEMU plumbing

```
qemu-system-aarch64 ... \
    -drive if=none,file=disk.img,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0
```
Create the image: `qemu-img create -f raw disk.img 64M`. Day 23 makes it ext2.

---

## 5. Pitfalls

1. **Endianness**: spec is little-endian; AArch64 little-endian → no swaps needed.
2. **DMB barriers**: between filling descriptors, writing `avail->idx`, and notifying — without `dmb ish` device may see stale data.
3. **virt_to_phys with stack-allocated `hdr` / `status`**: kernel stack is in direct-map → `virt_to_phys(va) = va - 0xffff000000000000UL`. Verify this with an assertion.
4. **Wrong `WRITE` flag direction**: the bit means "device writes" — so a *read* sets it on the data desc, a *write* clears it.
5. **Queue size**: must be a power of two for some devices; pick 16.

---

## 6. Verification

```c
u8 sec[512];
BUG_ON(block_read(0, 1, sec));
hexdump(sec, 32);     /* expect ext2 superblock magic 0xEF53 at offset 56 once Day 23 done */
```

Create a known-content disk:
```
$ dd if=/dev/urandom of=disk.img bs=512 count=128
```
Read sector 0, verify against host `xxd disk.img | head`.

---

## 7. Stretch

- Multi-queue support (`num_queues` feature).
- Indirect descriptors (saves slots when descriptor count is high).
- Async block layer (returns immediately, caller polls a completion).

---

## 8. References

- *Virtio v1.2 spec*, §4.2 (MMIO transport), §5.2 (block).
- Linux `drivers/virtio/virtio_mmio.c`, `drivers/block/virtio_blk.c`.
- QEMU `hw/block/virtio-blk.c`.
