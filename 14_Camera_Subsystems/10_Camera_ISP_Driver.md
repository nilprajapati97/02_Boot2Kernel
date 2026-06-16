# Module 10 — Camera ISP Driver

> **Scope:** How an Image Signal Processor is driven from the kernel. Covers ISP
> hardware architecture, the receive/process/write-master datapath, statistics and
> parameter DMA, the per-block configuration model, the two ISP driver patterns
> (kernel-visible vs firmware/black-box), interrupt and frame-done handling, and the
> 3A plumbing. Uses `rkisp1` (mainline, open) as the worked example and contrasts with
> Qualcomm IFE / NVIDIA ISP (Modules 11/12). This is the driver side of Module 4.

---

## 1. What an ISP driver does (and doesn't)

The ISP transforms RAW Bayer into YUV (Module 4). The **kernel ISP driver** does *not*
implement the image math — that's hardware/firmware. The driver:
- Registers ISP **sub-devices** + capture **video nodes** into the media graph.
- Configures ISP **hardware blocks** (enable/bypass + parameters) per frame.
- Sets up **input** (from CSID) and **output** (DMA write-masters to DDR) paths.
- Routes **statistics DMA** to userspace (the 3A loop, Module 4 §5).
- Handles **interrupts** (frame start/done, errors) and completes vb2 buffers.

```
 RAW from CSID ─► ISP input ─► [HW blocks] ─► resizer ─► write-master ─► DDR (vb2)
                                   │                                       ▲
                                   └─► stats engine ─► stats DMA ─► meta node (3A)
                       params from meta node ──────────► block config
```

---

## 2. ISP hardware architecture

```
 ┌──────────────────────────────────────────────────────────────┐
 │                          ISP CORE                              │
 │  input  ┌─────┐ ┌─────┐ ┌──────┐ ┌────┐ ┌────┐ ┌─────┐         │
 │  (RAW) ─►│ BLC │►│ LSC │►│demosa│►│ CCM│►│gamma│►│ CSC │─┐       │
 │         └─────┘ └─────┘ └──────┘ └────┘ └────┘ └─────┘ │       │
 │            │       │        │                          ▼       │
 │         ┌──┴───────┴────────┴──┐               ┌──────────────┐│
 │         │   STATS ENGINE       │               │  RESIZER /   ││
 │         │  (AE/AWB/AF/hist)    │               │  SCALER/CROP ││
 │         └──────────┬───────────┘               └──────┬───────┘│
 └────────────────────┼──────────────────────────────────┼───────┘
              stats DMA│                      write-master │
                       ▼                                   ▼
                  meta video node                   capture video node(s)
                  (DDR stats buffer)                (DDR frame buffer, vb2)
```

ISP blocks are configured via memory-mapped registers. Many ISPs have **shadow/double-
buffered registers**: you write the next frame's config, and it latches at the next
frame boundary so changes are coherent per-frame (the hardware analog of the Request
API, Module 6).

---

## 3. The two ISP driver patterns

```
 PATTERN A — Kernel-visible ISP (mainline, open)
 ───────────────────────────────────────────────
 - Each block configured via registers the kernel driver owns.
 - Tuning params + 3A stats exchanged through V4L2 META nodes.
 - 3A algorithms run in userspace (libcamera IPA) or a simple kernel default.
 - Examples: rkisp1 (Rockchip), i.MX (ISI/ISP), Allwinner, Mediatek (partly).

 PATTERN B — Firmware / black-box ISP
 ─────────────────────────────────────
 - ISP controlled by a firmware on a DSP/microcontroller.
 - Kernel sends opaque config blobs + RAW; firmware does processing + 3A.
 - Examples: Qualcomm (CamX/CHI over CAMSS+ICP), NVIDIA (Argus/RCE).
 - Kernel driver = data mover + buffer/IRQ manager + firmware mailbox.
```

Knowing which pattern your SoC uses tells you *where the image-quality logic lives* and
what you can debug from the kernel. Mainline favors A; production phones use B.

---

## 4. Worked example: rkisp1 (the open reference)

`drivers/media/platform/rockchip/rkisp1/` is the best-documented open ISP driver.
Its media graph:

```
 sensor → rkisp1_isp (subdev) → ┬─ rkisp1_resizer_mainpath → rkisp1_mainpath (video)
                                ├─ rkisp1_resizer_selfpath → rkisp1_selfpath (video)
                                ├─ rkisp1_params  (meta OUTPUT  node)  ← tuning params
                                └─ rkisp1_stats   (meta CAPTURE node)  → 3A stats
```

- `rkisp1-isp.c`   — the ISP subdev: input format, crop, block enable.
- `rkisp1-resizer.c` — scaler/crop subdevs for main and self paths.
- `rkisp1-capture.c` — the DMA capture video nodes (vb2 queues).
- `rkisp1-params.c` — accepts per-frame tuning params (UAPI struct).
- `rkisp1-stats.c`  — delivers AE/AWB/AF/histogram stats to userspace.

UAPI: `include/uapi/linux/rkisp1-config.h` defines the exact param/stats structs — read
this to understand the kernel/userspace 3A contract.

```c
/* userspace fills params and queues them on the params meta node */
struct rkisp1_params_cfg {
    __u32 module_en_update;     /* which blocks to enable/disable */
    __u32 module_ens;
    __u32 module_cfg_update;    /* which block configs are valid */
    struct rkisp1_cif_isp_isp_other_cfg others;  /* BLC, LSC, AWB gains, CCM... */
    struct rkisp1_cif_isp_isp_meas_cfg meas;     /* AE/AWB/AF measurement windows */
};
```

---

## 5. Configuring ISP blocks per frame

The params meta node carries the per-frame block configuration:

```c
/* rkisp1-params.c: apply userspace params into ISP registers */
static void rkisp1_params_apply(struct rkisp1_params *params,
                                struct rkisp1_params_cfg *cfg)
{
    if (cfg->module_cfg_update & RKISP1_CIF_ISP_MODULE_BLS)
        rkisp1_bls_config(params, &cfg->others.bls_config);   /* black level */
    if (cfg->module_cfg_update & RKISP1_CIF_ISP_MODULE_LSC)
        rkisp1_lsc_config(params, &cfg->others.lsc_config);   /* lens shading */
    if (cfg->module_cfg_update & RKISP1_CIF_ISP_MODULE_AWB_GAIN)
        rkisp1_awb_gain_config(params, &cfg->others.awb_gain_config);
    if (cfg->module_cfg_update & RKISP1_CIF_ISP_MODULE_CTK)
        rkisp1_ctk_config(params, &cfg->others.ctk_config);   /* color matrix */
    /* enable/disable blocks */
    rkisp1_module_config_update(params, cfg);
}
```

Writes land in shadow registers and latch at the next frame boundary. This is the
kernel side of Module 4's pipeline: each `*_config` programs one stage (BLC, LSC, AWB,
CCM, gamma...).

---

## 6. Statistics DMA and the 3A loop

```c
/* rkisp1-stats.c: on frame-done IRQ, read measurement registers / stats buffer */
static void rkisp1_stats_isr(struct rkisp1_stats *stats, u32 isp_ris)
{
    struct rkisp1_stat_buffer *cur = current_stat_buffer(stats);

    if (isp_ris & RKISP1_CIF_ISP_AWB_DONE)
        rkisp1_stats_get_awb_meas(stats, cur);   /* per-region R/G/B sums */
    if (isp_ris & RKISP1_CIF_ISP_AFM_FIN)
        rkisp1_stats_get_afc_meas(stats, cur);   /* AF contrast */
    if (isp_ris & RKISP1_CIF_ISP_EXP_END)
        rkisp1_stats_get_aec_meas(stats, cur);   /* exposure histogram */

    vb2_buffer_done(&cur->vb.vb2_buf, VB2_BUF_STATE_DONE);   /* deliver to userspace */
}
```

```
 ISP stats IRQ ─► fill stats buffer ─► vb2_buffer_done ─► userspace DQBUF (meta)
        │                                                       │
        │                                              3A algorithm (libcamera IPA)
        │                                                       │
        ▼                                                       ▼
 next frame's params  ◄── meta OUTPUT QBUF ◄── new exposure/gain/WB/focus
 + sensor exposure/gain via V4L2 controls (Module 9), tagged via Request API
```

The kernel's job is purely **transport + timing**: collect stats, deliver them, accept
params, apply them on the right frame. The intelligence (AE/AWB/AF) is in userspace
(libcamera) for Pattern A, or firmware for Pattern B.

---

## 7. Input and output path setup

```c
/* ISP subdev start: program input format/crop from the CSID-side pad */
static int rkisp1_isp_s_stream(struct v4l2_subdev *sd, int enable)
{
    if (enable) {
        rkisp1_config_isp(isp);          /* input data type, width/height, crop */
        rkisp1_isp_start(isp);           /* enable ISP core + IRQs */
        v4l2_subdev_call(sensor, video, s_stream, 1);   /* start upstream */
    } else {
        v4l2_subdev_call(sensor, video, s_stream, 0);
        rkisp1_isp_stop(isp);
    }
    return 0;
}

/* Capture video node start: program the write-master DMA address */
static int rkisp1_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
    rkisp1_cap_config(cap);              /* output format, stride, planes */
    rkisp1_set_next_buffer(cap);         /* program DMA dst = first vb2 buffer */
    rkisp1_cap_enable(cap);              /* enable write-master + frame IRQ */
    return v4l2_pipeline_pm_get(&cap->vnode.vdev.entity);
}
```

The write-master DMA destination is reprogrammed each frame from the vb2 buffer queue
(Module 13). Multiple output paths (main = full-res for record, self = downscaled for
preview) each have their own write-master and video node.

---

## 8. Interrupt and frame-done handling

```c
static irqreturn_t rkisp1_isp_isr(int irq, void *ctx)
{
    u32 status = readl(base + ISP_RIS);     /* raw interrupt status */

    if (status & ISP_FRAME_START)
        handle_frame_start(isp);            /* SOF: latch shadow regs, params */
    if (status & ISP_FRAME_DONE)
        handle_frame_done(isp);             /* complete frame, swap buffers */
    if (status & (ISP_DATA_LOSS | ISP_PIC_SIZE_ERR))
        handle_isp_error(isp, status);      /* count + recover */

    writel(status, base + ISP_ICR);         /* clear */
    return IRQ_HANDLED;
}
```

```
 SOF (frame start) ─► latch new params/shadow regs for this frame
 EOF (frame done)  ─► write-master finished ─► vb2_buffer_done(buf, DONE)
                      ─► program next vb2 buffer into write-master
 ERROR (overflow/size) ─► increment counters, may need pipeline restart
```

The 2-3 frame 3A latency (Module 4 §7) comes from this: stats from EOF(N) → userspace →
params QBUF → latched at SOF(N+2).

---

## 9. Qualcomm IFE vs NVIDIA ISP (contrast)

```
 Qualcomm (Module 11): CSID → IFE (Image Front End) does BLC/demosaic/scale/stats
   in hardware, but tuning + 3A run in CamX/CHI userspace via the ICP firmware.
   Kernel CAMSS driver feeds IFE, manages write-masters (RDI/PIX paths), SMMU, IRQs.

 NVIDIA (Module 12): NVCSI → VI → ISP, with the ISP driven by RCE firmware; the
   libargus userspace stack runs 3A. Kernel tegra-video driver moves data + buffers.
```

Both are **Pattern B**: the kernel ISP driver is mostly a buffer/DMA/IRQ manager and
firmware mailbox; the image-quality logic is proprietary. rkisp1 (Pattern A) is what you
study to actually *see* the block configuration in open code.

---

## 10. Debugging the ISP layer

```bash
# Topology and which ISP nodes exist
media-ctl -p -d /dev/media0

# Capture processed frames
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
         --stream-mmap --stream-count=60

# Capture/inspect 3A stats (meta node)
v4l2-ctl -d /dev/video2 --stream-mmap --stream-count=10  # stats meta node

# ISP error counters / dynamic debug
echo 'module rkisp1 +p' > /sys/kernel/debug/dynamic_debug/control
dmesg | grep -iE "rkisp1|isp|overflow|data loss|size err|stats"

# Bypass ISP: capture RAW directly from CSID (if RDI/RAW path exists)
#   isolates sensor/CSI problems from ISP problems
```

Symptom → cause:
```
 Correct RAW but bad YUV         → ISP block config (params) wrong, not sensor/CSI
 Stats never arrive              → stats meta node not queued, or stats IRQ masked
 Frame-done but buffers not done → write-master not programmed / wrong DMA addr
 ISP overflow/data-loss errors   → input rate > ISP throughput or DDR backpressure
 Params have no effect           → applied to wrong frame / shadow latch timing
 Wrong colors only               → CCM/AWB params; isolate by capturing RAW
```

The key isolation technique: **capture RAW from the CSID bypass path**. If RAW is good
but YUV is bad, the problem is in the ISP config, not the sensor/CSI.

---

## 11. Interview Q&A

**Q1. Does the kernel ISP driver implement demosaic/AWB/etc.?**
No. The image-processing math runs in ISP hardware (and, for 3A tuning, in
userspace/firmware). The kernel driver configures the hardware blocks (enable/params),
routes DMA, handles interrupts, and delivers stats — it's transport and control, not the
algorithms.

**Q2. Contrast the two ISP driver patterns.**
Pattern A (kernel-visible, e.g. rkisp1): the kernel owns block registers and exchanges
tuning params + 3A stats via V4L2 metadata nodes; 3A runs in userspace (libcamera IPA).
Pattern B (firmware, e.g. Qualcomm/NVIDIA): a firmware/DSP does processing and 3A; the
kernel sends opaque blobs and moves buffers. A is debuggable in open code; B is mostly a
black box.

**Q3. How do ISP statistics reach the 3A algorithm and how do results return?**
On frame-done IRQ the driver fills a stats buffer and `vb2_buffer_done`s it on a META
CAPTURE node; userspace DQBUFs it, the 3A algorithm computes new settings, and returns
ISP params via a META OUTPUT node (QBUF) plus sensor exposure/gain via V4L2 controls —
tagged to a frame via the Request API.

**Q4. Why are ISP registers often double-buffered/shadowed?**
So a full frame's configuration changes atomically. You write the next frame's params to
shadow registers; the hardware latches them at the frame boundary (SOF), guaranteeing no
mid-frame mixing of old/new settings — the hardware counterpart of per-frame Request API
semantics.

**Q5. Explain the 2-3 frame latency in ISP 3A from the IRQ timeline.**
Stats are produced at EOF(N). Userspace reads them, computes settings, and QBUFs params,
which can only latch at the next safe boundary, SOF(N+2), because frame N+1 is already
in flight. Hence settings derived from frame N affect frame N+2.

**Q6. The RAW looks correct but the YUV output is wrong-colored. Where's the bug?**
In the ISP, not the sensor/CSI. Correct RAW means capture/transport is fine; wrong color
points to AWB gains or the Color Correction Matrix in the ISP params. Capturing RAW from
the CSID bypass is exactly the isolation step that proves this.

**Q7. What causes ISP overflow / data-loss interrupts?**
The input pixel rate exceeds the ISP's processing throughput or the write-master can't
drain to DDR fast enough (DDR bandwidth/QoS starvation). It's a throughput/bandwidth
problem (Module 16), surfaced as overflow/data-loss error IRQs.

**Q8. On Qualcomm/NVIDIA, what is the kernel ISP driver responsible for?**
Data movement and control: feeding RAW/config to the IFE/ISP hardware, managing
write-master output paths and SMMU-mapped buffers, handling frame/error interrupts,
delivering done buffers and stats, and communicating with the ISP firmware (ICP/RCE).
The 3A and tuning math live in firmware/userspace (CamX, Argus).

---

### Key takeaways
- The kernel ISP driver configures hardware blocks, routes input/output DMA, handles
  IRQs, and plumbs 3A stats/params — it does not implement the image math.
- Two patterns: kernel-visible (rkisp1, params/stats via metadata nodes, libcamera 3A)
  vs firmware black-box (Qualcomm IFE/ICP, NVIDIA ISP/Argus).
- Shadow/double-buffered registers latch params per-frame; the SOF/EOF IRQ timeline
  produces the ~2-frame 3A latency.
- Isolate ISP bugs by capturing RAW from the CSID bypass: good RAW + bad YUV = ISP
  config problem.
