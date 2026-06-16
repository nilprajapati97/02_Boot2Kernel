# Module 6 — The V4L2 Framework

> **Scope:** Video4Linux2 in depth — the API and kernel framework userspace uses to
> capture video. Covers video devices vs sub-devices, the ioctl model, formats,
> controls, buffer management (videobuf2 from the V4L2 angle), the streaming state
> machine, selection/crop, events, metadata, memory models (MMAP/USERPTR/DMABUF), and
> the Request API. This is the API surface every camera driver implements.

---

## 1. What V4L2 is and where it sits

V4L2 is the kernel↔userspace contract for video capture/output. It provides:
- Device nodes: `/dev/videoN` (video devices) and `/dev/v4l-subdevN` (sub-devices).
- An ioctl-based control/format/streaming API.
- The **videobuf2** buffer/DMA framework (Module 13).
- Integration with the **Media Controller** (Module 5) for complex pipelines.

```
 Userspace (v4l2-ctl, libcamera, GStreamer, Android HAL)
        │  ioctl() on /dev/videoN and /dev/v4l-subdevN
        ▼
 V4L2 core (drivers/media/v4l2-core/*)
        │  v4l2_ioctl_ops / v4l2_subdev_ops callbacks
        ▼
 Driver (sensor, CSI, ISP, capture)  ── videobuf2 ──► DMA ──► DDR
```

Core source: `drivers/media/v4l2-core/` (`v4l2-dev.c`, `v4l2-ioctl.c`,
`v4l2-ctrls*.c`, `v4l2-subdev.c`, `v4l2-fwnode.c`, `v4l2-async.c`).

---

## 2. video_device vs v4l2_subdev (recap + API)

```c
/* The video node (/dev/videoN): owns the vb2 queue, streams buffers */
struct video_device {
    const struct v4l2_ioctl_ops *ioctl_ops;
    struct v4l2_device *v4l2_dev;
    struct vb2_queue *queue;
    struct media_pad pad;
    struct mutex *lock;
    ...
};

/* The sub-device (/dev/v4l-subdevN): a processing block, pad-format based */
struct v4l2_subdev {
    const struct v4l2_subdev_ops *ops;     /* .core/.video/.pad op groups */
    struct media_entity entity;
    struct v4l2_ctrl_handler *ctrl_handler;
    struct v4l2_async_subdev asd;
    ...
};
```

`v4l2_subdev_ops` groups:
```c
struct v4l2_subdev_ops {
    const struct v4l2_subdev_core_ops    *core;   /* s_power, log_status... */
    const struct v4l2_subdev_video_ops   *video;  /* s_stream (start/stop!) */
    const struct v4l2_subdev_pad_ops     *pad;    /* set_fmt, enum_mbus_code,
                                                     get/set_selection, link_validate */
    ...
};
```

The all-important `s_stream` (start/stop the block) lives in `video_ops`; pad formats
in `pad_ops`. Sensor and CSI drivers implement these (Module 9).

---

## 3. The V4L2 ioctl model

A capture app's lifecycle:

```c
fd = open("/dev/video0", O_RDWR);

ioctl(fd, VIDIOC_QUERYCAP, &cap);            /* capabilities */
ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc);        /* list pixel formats */
ioctl(fd, VIDIOC_S_FMT, &format);            /* set resolution + pixfmt */
ioctl(fd, VIDIOC_REQBUFS, &req);             /* allocate N buffers (MMAP/DMABUF) */
ioctl(fd, VIDIOC_QUERYBUF, &buf);            /* get buffer offset → mmap() */
ioctl(fd, VIDIOC_QBUF, &buf);                /* enqueue empty buffers */
ioctl(fd, VIDIOC_STREAMON, &type);           /* start capture */
for (;;) {
    ioctl(fd, VIDIOC_DQBUF, &buf);           /* wait for a filled frame */
    process(buf);
    ioctl(fd, VIDIOC_QBUF, &buf);            /* recycle the buffer */
}
ioctl(fd, VIDIOC_STREAMOFF, &type);          /* stop */
```

The driver implements these via `struct v4l2_ioctl_ops`:
```c
static const struct v4l2_ioctl_ops capture_ioctl_ops = {
    .vidioc_querycap          = cap_querycap,
    .vidioc_enum_fmt_vid_cap  = cap_enum_fmt,
    .vidioc_g_fmt_vid_cap     = cap_g_fmt,
    .vidioc_s_fmt_vid_cap     = cap_s_fmt,
    .vidioc_reqbufs           = vb2_ioctl_reqbufs,    /* vb2 helpers */
    .vidioc_querybuf          = vb2_ioctl_querybuf,
    .vidioc_qbuf              = vb2_ioctl_qbuf,
    .vidioc_dqbuf             = vb2_ioctl_dqbuf,
    .vidioc_streamon          = vb2_ioctl_streamon,
    .vidioc_streamoff         = vb2_ioctl_streamoff,
};
```

Most buffer ioctls are handled by **vb2 helper functions** — you rarely write them.

---

## 4. Formats and the pixel-format model

```c
struct v4l2_pix_format {
    __u32 width;
    __u32 height;
    __u32 pixelformat;     /* V4L2_PIX_FMT_NV12, _SRGGB10, _YUYV ... (fourcc) */
    __u32 field;           /* progressive / interlaced */
    __u32 bytesperline;    /* stride — CRITICAL, must match HW alignment */
    __u32 sizeimage;       /* total buffer size */
    __u32 colorspace;
    ...
};
```

Two worlds:
- **Pixel formats** (`V4L2_PIX_FMT_*`, fourcc) at the **video node** — describe data in
  DDR (NV12, YUYV, packed RAW).
- **Media bus codes** (`MEDIA_BUS_FMT_*`) at **subdev pads** — describe data on the
  internal bus (SRGGB10_1X10, UYVY8_1X16). Module 3/5.

The driver maps between them. Getting **bytesperline/stride** and **sizeimage** wrong
(alignment, packed RAW) is a top cause of sheared/corrupted frames.

Multiplanar variant: `v4l2_pix_format_mplane` for formats with separate planes (e.g.
NV12 as two DMA buffers), used by most SoC capture drivers
(`V4L2_CAP_VIDEO_CAPTURE_MPLANE`).

---

## 5. The V4L2 control framework

Controls expose tunable parameters (exposure, gain, focus, brightness...).

```c
struct v4l2_ctrl_handler ctrls;
v4l2_ctrl_handler_init(&ctrls, 8);

v4l2_ctrl_new_std(&ctrls, &ops, V4L2_CID_EXPOSURE,    1, 65535, 1, 1000);
v4l2_ctrl_new_std(&ctrls, &ops, V4L2_CID_ANALOGUE_GAIN,0,  978, 1,    0);
v4l2_ctrl_new_int_menu(&ctrls, &ops, V4L2_CID_LINK_FREQ,
                       ARRAY_SIZE(link_freqs)-1, 0, link_freqs);
subdev->ctrl_handler = &ctrls;

/* the driver's apply callback */
static int sensor_s_ctrl(struct v4l2_ctrl *ctrl) {
    struct sensor *s = ctrl_to_sensor(ctrl);
    if (pm_runtime_get_if_in_use(s->dev) == 0)   /* only touch HW if powered */
        return 0;
    switch (ctrl->id) {
    case V4L2_CID_EXPOSURE:       ret = write_exposure(s, ctrl->val); break;
    case V4L2_CID_ANALOGUE_GAIN:  ret = write_gain(s, ctrl->val);     break;
    }
    pm_runtime_put(s->dev);
    return ret;
}
```

Userspace:
```bash
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls
v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl exposure=2000,analogue_gain=128
```

Control classes: standard CIDs (`V4L2_CID_*`), plus driver-specific
(`V4L2_CID_USER_BASE`/custom). The framework handles ranges, defaults, volatility, and
`G/S/QUERY_CTRL` ioctls for you.

---

## 6. Buffer memory models (MMAP / USERPTR / DMABUF)

`VIDIOC_REQBUFS` chooses how buffer memory is provided:

```
V4L2_MEMORY_MMAP    : kernel allocates buffers; userspace mmap()s them.
                      Simplest, most common for capture.

V4L2_MEMORY_USERPTR : userspace provides malloc'd memory pointers.
                      Legacy, problematic with IOMMU/cache — avoid.

V4L2_MEMORY_DMABUF  : buffers are dma-buf fds, shared zero-copy with GPU/
                      encoder/display. The modern path for pipelines.
```

```
 DMABUF zero-copy sharing:
 ┌────────┐  export fd  ┌────────────┐  import fd  ┌──────────────┐
 │ camera │────────────►│  dma-buf   │────────────►│ encoder/GPU  │
 │ (vb2)  │             │ (1 buffer) │             │ (no copy)    │
 └────────┘             └────────────┘             └──────────────┘
```

DMABUF + dma-heaps (Module 14) is how Android shares camera frames to the encoder and
display with zero copy.

---

## 7. The streaming state machine

```
   REQBUFS ─────► QUERYBUF ─────► QBUF (enqueue) ─────► STREAMON
                                     │                     │
                                     ▼                     ▼
                              [buffers QUEUED]      driver start_streaming()
                                     │              starts subdev s_stream(1)
                                     ▼                     │
                               DMA fills buffer            ▼
                                     │              frames flow sensor→ISP→DMA
                                     ▼
                                buffer DONE ──► DQBUF (userspace gets frame)
                                     │
                                     ▼
                              QBUF again (recycle)
                                     ...
   STREAMOFF ──► stop_streaming() ──► s_stream(0) ──► return all buffers
```

vb2 enforces this state machine (Module 13). Driver hooks:
```c
static const struct vb2_ops capture_qops = {
    .queue_setup     = cap_queue_setup,     /* validate count/size/planes */
    .buf_prepare     = cap_buf_prepare,     /* validate each buffer */
    .buf_queue       = cap_buf_queue,       /* add to driver's pending list */
    .start_streaming = cap_start_streaming, /* program HW, start pipeline */
    .stop_streaming  = cap_stop_streaming,  /* stop HW, return buffers */
};
```

`start_streaming` typically calls `video_device_pipeline_start()` (Module 5) then
`v4l2_subdev_call(sensor, video, s_stream, 1)` to start the sensor.

---

## 8. Selection API (crop / compose)

Replaces the legacy `VIDIOC_CROP`. Describes rectangles for crop (input) and compose
(output):

```c
struct v4l2_selection sel = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .target = V4L2_SEL_TGT_CROP,        /* or _COMPOSE, _CROP_BOUNDS... */
    .r = { .left=0, .top=0, .width=1280, .height=720 },
};
ioctl(fd, VIDIOC_S_SELECTION, &sel);
```

On subdevs, `VIDIOC_SUBDEV_S_SELECTION` sets per-pad crop/compose — how a sensor
windowing or ISP scaler crop is expressed (digital zoom, ROI). Tied to Bayer-phase
caveats (Module 3).

---

## 9. Events

`VIDIOC_SUBSCRIBE_EVENT` / `DQEVENT` deliver asynchronous notifications:
```
V4L2_EVENT_FRAME_SYNC   : frame start (SOF) — used for sync/timing
V4L2_EVENT_CTRL         : a control changed (e.g. auto-exposure updated gain)
V4L2_EVENT_EOS          : end of stream (encoders)
V4L2_EVENT_SOURCE_CHANGE: input resolution changed (HDMI-in, etc.)
```
3A/HAL uses `FRAME_SYNC` to align settings with frames.

---

## 10. Metadata video nodes

ISP **statistics** and **parameters** flow through dedicated metadata buffers:
```
V4L2_BUF_TYPE_META_CAPTURE   : kernel→user (ISP 3A stats, embedded data)
V4L2_BUF_TYPE_META_OUTPUT    : user→kernel (ISP tuning params)
V4L2_CAP_META_CAPTURE        : device capability
```
This is how rkisp1 (and others) deliver AE/AWB/AF stats and accept per-frame ISP
params — the kernel side of the 3A plumbing from Module 4.

---

## 11. The Request API (atomic per-frame configuration)

Problem: settings (exposure, gain, ISP params) must apply to a **specific** future
frame, atomically. Solution: the **Request API**.

```c
/* allocate a request from the media device */
ioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &request_fd);

/* queue controls/buffers against the request instead of applying immediately */
buf.flags |= V4L2_BUF_FLAG_REQUEST_FD;
buf.request_fd = request_fd;
ioctl(video_fd, VIDIOC_QBUF, &buf);
/* set controls with request_fd too via VIDIOC_S_EXT_CTRLS + which=REQUEST_VAL */

/* submit: all controls+buffer apply together to the same frame */
ioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL);
/* later */
ioctl(request_fd, MEDIA_REQUEST_IOC_REINIT, NULL);  /* reuse */
```

Driver side implements `req_validate`, `req_queue` (`struct media_device_ops` /
`vb2`/ctrl request handlers). This guarantees "this exposure + this gain + this buffer"
all belong to frame N — essential for the 3A loop and HDR multi-frame, and central to
Android Camera HAL3's per-request model (Module 17).

---

## 12. async subdev registration and fwnode

Sensor subdevs probe asynchronously (I2C order isn't guaranteed). V4L2 **async**
framework binds them to the bridge (CSI/ISP) when both are ready, using **fwnode**
(device tree) endpoint matching:

```c
/* bridge (CSI/ISP) registers a notifier listing expected sensors */
v4l2_async_nf_init(&notifier, &csi->v4l2_dev);
v4l2_async_nf_add_fwnode_remote(&notifier, csi_ep_fwnode,
                                struct v4l2_async_connection);
notifier.ops = &csi_async_ops;     /* .bound, .complete */
v4l2_async_nf_register(&notifier);

/* sensor registers itself */
v4l2_async_register_subdev_sensor(&sensor->sd);
```

When all expected subdevs appear, `.complete()` fires and the driver finalizes the
media graph + registers video nodes. DT endpoints (Module 7) drive the matching.

---

## 13. Debugging V4L2

```bash
v4l2-ctl --all -d /dev/video0                 # caps, formats, current settings
v4l2-ctl --list-formats-ext -d /dev/video0    # all pixfmts + sizes + fps
v4l2-ctl --stream-mmap --stream-count=100 -d /dev/video0  # smoke-test capture
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls     # subdev controls/ranges

# Enable V4L2 core debug (dmesg traces of every ioctl):
echo 0x3 > /sys/class/video4linux/video0/dev_debug

# yavta — fine-grained buffer/streaming testing
yavta -f NV12 -s 1920x1080 -n 4 --capture=100 /dev/video0
```

Common V4L2 bugs:
- `VIDIOC_S_FMT` returns a *different* format than requested → driver adjusted it
  (always re-read the returned struct; don't assume).
- DQBUF blocks forever → no frames arriving (pipeline not started, sensor not
  streaming, CSI errors) — check `media-ctl -p` + dmesg.
- `-EPIPE` on STREAMON → pipeline format mismatch (Module 5).
- Sheared image → wrong bytesperline/stride.

---

## 14. Interview Q&A

**Q1. Walk through the V4L2 capture ioctl sequence.**
`QUERYCAP → ENUM_FMT → S_FMT → REQBUFS → QUERYBUF+mmap → QBUF(all) → STREAMON →
loop(DQBUF→process→QBUF) → STREAMOFF`. REQBUFS allocates, QBUF enqueues empty buffers,
DQBUF returns filled ones.

**Q2. Difference between a pixel format and a media bus code?**
Pixel formats (`V4L2_PIX_FMT_*`, fourcc) describe data in memory at the video node
(NV12, packed RAW). Media bus codes (`MEDIA_BUS_FMT_*`) describe data on the internal
bus between subdev pads (SRGGB10_1X10). The driver maps between them.

**Q3. Compare MMAP, USERPTR, and DMABUF memory models.**
MMAP: kernel allocates, userspace maps — simplest, common. USERPTR: userspace supplies
pointers — legacy, IOMMU/cache-unfriendly. DMABUF: buffers are dma-buf fds shared
zero-copy with GPU/encoder/display — the modern pipeline path.

**Q4. Why does the Request API exist?**
To apply a set of controls + a buffer atomically to a specific future frame. The 3A
loop and HDR need "this exposure + this gain + this buffer = frame N." Without it,
settings could land on the wrong frame due to pipeline latency.

**Q5. Where is `s_stream` and what does it do?**
In `v4l2_subdev_video_ops`. Called to start/stop a subdev's data flow. The capture
driver's `start_streaming` calls it down the chain (ISP→CSI→sensor) to begin
streaming, and with 0 to stop.

**Q6. How are sensor subdevs bound to the CSI bridge given async probe order?**
Via the V4L2 async framework + fwnode: the bridge registers a notifier listing
expected remote endpoints (from DT), sensors register themselves; when all appear, the
`.complete()` callback finalizes the graph and registers video nodes.

**Q7. S_FMT returns a different resolution than you asked for. Bug?**
No — V4L2 lets drivers adjust to the nearest supported format. Userspace must read back
the returned `v4l2_format` and use those actual values; never assume the request was
honored verbatim.

**Q8. How do ISP statistics reach userspace in mainline V4L2?**
Through metadata video nodes (`V4L2_BUF_TYPE_META_CAPTURE`): the ISP DMAs stats into
metadata buffers that userspace DQBUFs, while tuning params go back via
`META_OUTPUT`. This is the kernel side of the 3A loop.

---

### Key takeaways
- V4L2 = ioctl API + vb2 buffers + control framework; video nodes stream buffers,
  subdevs carry pad formats.
- The capture state machine (REQBUFS→QBUF→STREAMON→DQBUF/QBUF loop) is enforced by vb2.
- DMABUF enables zero-copy sharing; the Request API enables atomic per-frame settings.
- async/fwnode binds sensor subdevs to the bridge; metadata nodes carry 3A stats/params.
- Most buffer ioctls are vb2 helpers — you implement formats, controls, and the vb2_ops
  start/stop hooks.
