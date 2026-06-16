# Module 13 — The videobuf2 Framework

> **Scope:** videobuf2 (vb2) — the kernel framework that manages video buffers, their
> memory, DMA mapping, and the streaming state machine for every V4L2 capture/output
> driver. Covers the `vb2_queue`, the buffer lifecycle/state machine, the three memory
> models (MMAP/USERPTR/DMABUF), the memory-allocator backends (vmalloc, dma-contig,
> dma-sg), cache coherency, the driver's `vb2_ops`, and how frames flow from DMA-done
> IRQ to userspace DQBUF. This is the buffer engine under Module 6.

---

## 1. Why vb2 exists

Every camera driver needs to: allocate frame buffers, map them for DMA, hand empty ones
to hardware, get filled ones back on IRQ, and deliver them to userspace — correctly,
with cache coherency and a strict state machine. vb2 provides all of this so drivers
don't reimplement it.

```
 Userspace            vb2 core                 Driver              Hardware
 ─────────            ────────                 ──────              ────────
 REQBUFS    ──►  allocate buffers (backend) ──► queue_setup
 QBUF       ──►  state QUEUED               ──► buf_queue ──────► DMA dst programmed
 STREAMON   ──►                             ──► start_streaming ─► HW starts
                                            ◄── vb2_buffer_done ◄─ DMA-done IRQ
 DQBUF      ◄──  state DONE                 (frame delivered)
```

Source: `drivers/media/common/videobuf2/` (`videobuf2-core.c`, `-v4l2.c`,
`-dma-contig.c`, `-dma-sg.c`, `-vmalloc.c`, `-dma-buf.c`).

---

## 2. The vb2_queue — central object

```c
struct vb2_queue {
    unsigned int type;             /* V4L2_BUF_TYPE_VIDEO_CAPTURE(_MPLANE) */
    unsigned int io_modes;         /* VB2_MMAP | VB2_USERPTR | VB2_DMABUF */
    struct device *dev;            /* DMA device (for mapping) */
    const struct vb2_ops *ops;     /* driver callbacks */
    const struct vb2_mem_ops *mem_ops;  /* allocator backend (dma-contig/sg/vmalloc) */
    void *drv_priv;                /* driver context */
    unsigned int min_buffers_needed; /* min to start streaming */
    struct mutex *lock;
    gfp_t gfp_flags;               /* GFP_DMA32 etc. */
    unsigned int buf_struct_size;  /* size of driver's per-buffer struct */
    ...
};
```

A driver initializes one queue per video node:

```c
q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
q->io_modes = VB2_MMAP | VB2_DMABUF;
q->dev = cam->dev;
q->ops = &cam_vb2_ops;
q->mem_ops = &vb2_dma_contig_memops;   /* or dma_sg / vmalloc */
q->drv_priv = cam;
q->buf_struct_size = sizeof(struct cam_buffer);
q->min_buffers_needed = 2;             /* need ≥2 for ping-pong */
q->lock = &cam->lock;
vb2_queue_init(q);
```

---

## 3. The driver's per-buffer struct

Drivers embed `vb2_v4l2_buffer` as the first member of their own buffer struct so they
can attach a list node and DMA addresses:

```c
struct cam_buffer {
    struct vb2_v4l2_buffer vb;     /* MUST be first */
    struct list_head list;          /* driver's pending/active list */
    dma_addr_t dma_addr;            /* DMA address for the write-master */
};
#define to_cam_buffer(vbuf) container_of(vbuf, struct cam_buffer, vb)
```

---

## 4. The vb2_ops (driver callbacks)

```c
static const struct vb2_ops cam_vb2_ops = {
    .queue_setup     = cam_queue_setup,
    .buf_prepare     = cam_buf_prepare,
    .buf_queue       = cam_buf_queue,
    .start_streaming = cam_start_streaming,
    .stop_streaming  = cam_stop_streaming,
    .wait_prepare    = vb2_ops_wait_prepare,    /* release lock while waiting */
    .wait_finish     = vb2_ops_wait_finish,
};
```

### queue_setup — validate/negotiate buffer count and sizes
```c
static int cam_queue_setup(struct vb2_queue *q,
                           unsigned int *nbuffers, unsigned int *nplanes,
                           unsigned int sizes[], struct device *alloc_devs[])
{
    struct cam *cam = vb2_get_drv_priv(q);
    unsigned int size = cam->format.fmt.pix_mp.plane_fmt[0].sizeimage;

    if (*nplanes) {                 /* called with proposed config: validate */
        if (sizes[0] < size) return -EINVAL;
        return 0;
    }
    *nplanes = 1;                   /* first call: tell vb2 our requirements */
    sizes[0] = size;
    if (*nbuffers < 2) *nbuffers = 2;
    return 0;
}
```

### buf_prepare — per-buffer validation before queueing
```c
static int cam_buf_prepare(struct vb2_buffer *vb)
{
    struct cam *cam = vb2_get_drv_priv(vb->vb2_queue);
    unsigned int size = cam->format...sizeimage;

    if (vb2_plane_size(vb, 0) < size) return -EINVAL;
    vb2_set_plane_payload(vb, 0, size);
    /* cache the DMA address for the hardware write-master */
    to_cam_buffer(to_vb2_v4l2_buffer(vb))->dma_addr =
        vb2_dma_contig_plane_dma_addr(vb, 0);
    return 0;
}
```

### buf_queue — hand an empty buffer to the driver
```c
static void cam_buf_queue(struct vb2_buffer *vb)
{
    struct cam *cam = vb2_get_drv_priv(vb->vb2_queue);
    struct cam_buffer *buf = to_cam_buffer(to_vb2_v4l2_buffer(vb));
    unsigned long flags;

    spin_lock_irqsave(&cam->qlock, flags);
    list_add_tail(&buf->list, &cam->buf_list);   /* pending list for the DMA */
    spin_unlock_irqrestore(&cam->qlock, flags);
}
```

### start_streaming / stop_streaming
```c
static int cam_start_streaming(struct vb2_queue *q, unsigned int count)
{
    struct cam *cam = vb2_get_drv_priv(q);
    int ret;

    ret = video_device_pipeline_start(&cam->vdev, &cam->pipe);  /* Module 5 */
    if (ret) goto err_return_buffers;

    cam_program_next_buffer(cam);          /* DMA dst = first pending buffer */
    ret = v4l2_subdev_call(cam->source, video, s_stream, 1);    /* start chain */
    if (ret) goto err_stop_pipe;
    return 0;

err_stop_pipe:
    video_device_pipeline_stop(&cam->vdev);
err_return_buffers:
    cam_return_all_buffers(cam, VB2_BUF_STATE_QUEUED);   /* give buffers back */
    return ret;
}

static void cam_stop_streaming(struct vb2_queue *q)
{
    struct cam *cam = vb2_get_drv_priv(q);
    v4l2_subdev_call(cam->source, video, s_stream, 0);
    video_device_pipeline_stop(&cam->vdev);
    cam_return_all_buffers(cam, VB2_BUF_STATE_ERROR);    /* MUST return all! */
}
```

**Critical rule:** `stop_streaming` must return *every* buffer it owns (via
`vb2_buffer_done` with ERROR/QUEUED) or userspace's DQBUF hangs forever and the queue
can't be freed. A frequent driver bug.

---

## 5. The completion path (DMA-done IRQ)

```c
static irqreturn_t cam_irq(int irq, void *data)
{
    struct cam *cam = data;
    struct cam_buffer *done;
    unsigned long flags;

    if (frame_done_status(cam)) {
        spin_lock_irqsave(&cam->qlock, flags);
        done = list_first_entry(&cam->buf_list, struct cam_buffer, list);
        list_del(&done->list);
        spin_unlock_irqrestore(&cam->qlock, flags);

        done->vb.vb2_buf.timestamp = ktime_get_ns();
        done->vb.sequence = cam->sequence++;
        done->vb.field = V4L2_FIELD_NONE;

        vb2_buffer_done(&done->vb.vb2_buf, VB2_BUF_STATE_DONE);  /* → userspace */

        cam_program_next_buffer(cam);   /* program next pending buffer (ping-pong) */
    }
    return IRQ_HANDLED;
}
```

```
 DMA fills buffer ─► IRQ ─► vb2_buffer_done(DONE) ─► userspace DQBUF wakes
                          └► program next buffer into write-master (no gap)
```

For tear-free continuous capture, the driver keeps **≥2 buffers** in flight (ping-pong):
while one is being DMA'd, the next is already programmed.

---

## 6. The buffer state machine

```
        ┌──────────┐  buf_prepare  ┌─────────┐  buf_queue  ┌────────┐
 alloc ►│ DEQUEUED │──────────────►│PREPARED │────────────►│ QUEUED │
        └──────────┘               └─────────┘             └───┬────┘
             ▲                                                 │ DMA fills
             │ DQBUF                                           ▼
        ┌────┴─────┐    vb2_buffer_done(DONE/ERROR)      ┌───────────┐
        │   DONE   │◄────────────────────────────────────│  ACTIVE   │
        └──────────┘                                     └───────────┘
```

vb2 enforces legal transitions; illegal QBUF/DQBUF ordering returns `-EINVAL`. On
STREAMOFF all buffers return to DEQUEUED.

---

## 7. Memory models (the V4L2 side, recap from Module 6)

```
 VB2_MMAP    : vb2 allocates buffers via mem_ops; userspace mmap()s them.
 VB2_USERPTR : userspace passes a pointer; vb2 pins/maps user pages (legacy).
 VB2_DMABUF  : userspace passes a dma-buf fd; vb2 attaches & maps it (zero-copy).
```

The driver advertises which it supports via `q->io_modes`. Modern camera pipelines use
**MMAP** for simple capture and **DMABUF** for zero-copy sharing to encoder/GPU/display.

---

## 8. Allocator backends (mem_ops)

vb2 separates the *model* (MMAP/DMABUF) from the *allocator backend*:

```
 vb2_dma_contig_memops : physically contiguous DMA memory (dma_alloc_*).
                         For hardware WITHOUT scatter-gather (needs contiguous).
                         Common for camera ISP write-masters; may need CMA (Module 14).

 vb2_dma_sg_memops     : scatter-gather — non-contiguous pages + an SG table.
                         For hardware WITH an IOMMU/SMMU or SG-capable DMA.
                         Avoids large contiguous allocation pressure.

 vb2_vmalloc_memops    : vmalloc'd memory, CPU-only (no DMA). For software/test
                         drivers (e.g. vivid), not real camera DMA.
```

Choosing the right backend is a **hardware** decision:
- ISP write-master with no IOMMU and contiguous requirement → **dma-contig** (+CMA).
- DMA behind an SMMU (Qualcomm/NVIDIA/most ARM64) → **dma-sg** works, SMMU makes
  scattered pages look contiguous to the device (Module 14).

```c
dma_addr_t addr = vb2_dma_contig_plane_dma_addr(vb, 0);   /* contig backend */
struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, 0);      /* sg backend */
```

---

## 9. Cache coherency

For non-coherent DMA (typical on ARM), vb2 handles cache maintenance around DMA:

```
 Before DMA (device reads/writes):  cache clean/invalidate as needed
 After DMA (CPU reads frame):        cache invalidate so CPU sees fresh data

 vb2 does this automatically based on buffer direction (capture = device writes,
 CPU reads → invalidate on done). Drivers can hint with V4L2_FLAG_MEMORY_NON_CONSISTENT
 / V4L2_BUF_FLAG_NO_CACHE_* to skip sync for performance when safe.
```

Cache bugs manifest as **stale/garbage image content** even though the DMA address is
correct — a subtle, hard-to-spot class of bug. If you bypass vb2's cache handling for
performance, you own the coherency (Module 14).

---

## 10. The full data flow (one frame)

```
 1. REQBUFS(4, MMAP) → vb2 allocs 4 dma-contig buffers, exposes mmap offsets
 2. QBUF×4           → each buf: buf_prepare (DMA addr cached) + buf_queue (listed)
 3. STREAMON         → start_streaming: pipeline_start, program buf0 to write-master,
                       s_stream(1) down to sensor
 4. sensor→CSI→ISP→DMA fills buf0
 5. frame-done IRQ   → vb2_buffer_done(buf0, DONE); program buf1
 6. DQBUF            → userspace gets buf0 (mmap'd, cache-invalidated)
 7. process; QBUF buf0 again → re-queued to the tail
 8. ...steady state: ping-pong across the 4 buffers...
 9. STREAMOFF        → stop_streaming: s_stream(0), pipeline_stop, return all buffers
```

---

## 11. Debugging vb2

```bash
# Smoke-test the queue end-to-end
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=100      # MMAP path
v4l2-ctl -d /dev/video0 --stream-dmabuf ...                   # DMABUF path
yavta -n 4 --capture=100 -f SRGGB10 -s 1920x1080 /dev/video0  # fine control

# vb2 debug (verbosity of buffer state transitions)
echo 3 > /sys/module/videobuf2_common/parameters/debug
dmesg | grep -iE "vb2|qbuf|dqbuf|buffer done|state"
```

Symptom → cause:
```
 DQBUF blocks forever            → no frames (pipeline not started / no IRQ), OR
                                    stop_streaming didn't return buffers
 -ENOMEM on REQBUFS              → dma-contig needs contiguous mem; CMA too small (Mod14)
 Garbage/stale image, addr OK    → cache coherency (wrong backend or skipped sync)
 Sheared image                   → bytesperline/sizeimage wrong in queue_setup
 "buffer for state X" warnings   → driver returned a buffer twice or wrong state
 Hang on STREAMOFF               → start_streaming error path didn't return buffers
```

---

## 12. Interview Q&A

**Q1. What does videobuf2 provide that drivers would otherwise reimplement?**
Buffer allocation, the three memory models (MMAP/USERPTR/DMABUF), DMA mapping and cache
coherency, the buffer state machine, and the QBUF/DQBUF/STREAMON ioctl plumbing — so a
driver only implements `queue_setup`, `buf_prepare/queue`, and `start/stop_streaming`
plus its IRQ completion.

**Q2. Why must stop_streaming return every buffer it owns?**
Because userspace blocks in DQBUF waiting for buffers, and vb2 can't free or re-init the
queue while the driver still owns buffers. If `stop_streaming` doesn't `vb2_buffer_done`
all in-flight buffers (with ERROR/QUEUED), DQBUF hangs and STREAMOFF/close deadlocks.

**Q3. Compare dma-contig, dma-sg, and vmalloc backends.**
dma-contig: physically contiguous DMA memory for hardware that needs contiguity (no
IOMMU) — may require CMA. dma-sg: scatter-gather pages + SG table for IOMMU-backed or
SG-capable DMA, avoiding large contiguous allocations. vmalloc: CPU-only memory for
software/test drivers, not real DMA.

**Q4. How does the ping-pong (double-buffering) work and why ≥2 buffers?**
While the DMA write-master fills buffer A, the driver has already programmed buffer B as
the next destination. On A's done-IRQ it delivers A and programs the next pending buffer,
so there's never a gap where the hardware has no target — preventing dropped/torn frames.
That needs at least two buffers in flight.

**Q5. You get a correct DMA address but the captured image is garbage/stale. Likely
cause?**
Cache coherency: on non-coherent ARM DMA, if the CPU's cache isn't invalidated after the
device writes the buffer, the CPU reads stale cached data. Either the wrong backend is
used, or vb2's cache sync was bypassed for performance without proper manual maintenance.

**Q6. Walk the buffer state machine.**
DEQUEUED → (buf_prepare) PREPARED → (buf_queue) QUEUED → (DMA) ACTIVE → (vb2_buffer_done)
DONE → (DQBUF) DEQUEUED. STREAMOFF returns all to DEQUEUED. vb2 rejects illegal
transitions with -EINVAL.

**Q7. Where does the driver get the DMA address to program into hardware?**
In `buf_prepare` (or `buf_queue`), via the backend helper:
`vb2_dma_contig_plane_dma_addr(vb, plane)` for contig, or `vb2_dma_sg_plane_desc()` for
the SG table. It caches this in its per-buffer struct and writes it to the write-master
register when programming the next buffer.

**Q8. How does DMABUF mode enable zero-copy and what does vb2 do?**
Userspace passes a dma-buf fd (e.g. exported by the encoder/GPU) in QBUF. vb2 attaches to
the dma-buf and maps it into the camera device's DMA/SMMU context, so the camera writes
directly into the buffer the consumer reads — no intermediate copy. vb2 manages
attach/map/fence lifecycle.

---

### Key takeaways
- vb2 owns buffer memory, DMA mapping, cache coherency, and the state machine; the
  driver implements `queue_setup`, `buf_prepare/queue`, `start/stop_streaming`, and IRQ
  completion via `vb2_buffer_done`.
- Choose the backend by hardware: dma-contig (contiguous, no IOMMU, may need CMA), dma-sg
  (IOMMU/SG), vmalloc (software only).
- Keep ≥2 buffers in flight (ping-pong) for tear-free capture; always return all buffers
  in stop_streaming.
- Cache coherency bugs show as stale images with correct addresses; DMABUF mode gives
  zero-copy sharing to encoder/GPU/display.
