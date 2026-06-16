# Module 11 — Qualcomm Camera Architecture (CAMSS / Spectra)

> **Scope:** The Qualcomm camera subsystem end to end: the **CAMSS** hardware blocks
> (CSIPHY, CSID, IFE/VFE, CAMNOC, ICP, BPS/IPE), the mainline `qcom-camss` driver, and
> the downstream **Spectra / CamX / CHI** userspace stack. Covers the hardware
> datapath, the kernel driver structure, RDI vs PIX paths, CPAS/SMMU, power domains and
> clocks, the camera-request-manager model, and how it all maps to Modules 5–10.
> Qualcomm is one of your two target SoC vendors.

---

## 1. The big picture: two stacks

Qualcomm camera has a **mainline** path and a **production (downstream)** path:

```
 MAINLINE (upstream Linux):              PRODUCTION (Qualcomm BSP / Android):
 ┌───────────────────────┐              ┌──────────────────────────────────┐
 │ qcom-camss V4L2 driver │              │ CamX / CHI (userspace HAL3)       │
 │ (CSIPHY/CSID/VFE)      │              │   + ISP firmware on ICP/A-DSP      │
 │ RDI to DDR, basic PIX  │              ├──────────────────────────────────┤
 │ libcamera 3A (limited) │              │ Camera Request Manager (CRM) +    │
 └───────────────────────┘              │ kernel cam_* drivers (techpack)   │
                                         └──────────────────────────────────┘
```

- **Mainline `drivers/media/platform/qcom/camss`** supports CSIPHY→CSID→VFE with RAW
  dump (RDI) and limited PIX processing — good for RAW capture and understanding the
  hardware, used with libcamera.
- **Downstream "techpack" camera** (`drivers/media/platform/camx`-style `cam_*`
  drivers + CamX/CHI userspace) is what ships in phones: full Spectra ISP, 3A, HDR,
  multi-camera, via the **Camera Request Manager**.

Know both: interviews probe the mainline architecture *and* the CamX/CHI production model.

---

## 2. CAMSS hardware datapath

```
 Sensor ─MIPI─► CSIPHY ─► CSID ─┬─ RDI path ─────────────► IFE BUS WM ─► DDR (RAW)
                                │
                                └─ PIX path ─► IFE (VFE) ─► IFE BUS WM ─► DDR (YUV)
                                                  │  stats
                                                  ▼
                                            stats WM ─► DDR (3A stats)

 Offline reprocess: DDR (RAW) ─► BPS ─► IPE ─► DDR (YUV)   (via ICP firmware)
```

Blocks:
- **CSIPHY** — D-PHY/C-PHY receiver; lane + settle config (Module 8).
- **CSID** — CSI decoder; VC/DT demux, CRC/ECC, routes to RDI or PIX.
- **IFE / VFE** (Image Front End / Video Front End) — real-time ISP: demosaic, crop,
  scale, 3A stats, write-masters. "VFE" is the older name; "IFE" the newer Spectra one.
- **IFE-lite** — a cut-down IFE for RDI-only (RAW dump) paths, cheap parallel streams.
- **CAMNOC** — the camera Network-On-Chip / bus fabric to DDR (bandwidth + QoS).
- **CPAS** (Camera Power, Clock & Bandwidth Subsystem) — central resource manager for
  AHB/AXI clocks, voting bandwidth, and SMMU setup.
- **ICP** (Image Control Processor) — an A-DSP/microcontroller running ISP firmware for
  offline reprocessing (BPS/IPE).
- **BPS** (Bayer Processing Segment) — offline RAW→Bayer-domain processing (HDR merge,
  denoise) for snapshots.
- **IPE** (Image Processing Engine) — offline YUV-domain processing (noise reduction,
  sharpening, color) for snapshots/video.
- **JPEG / LRME / CVP** — JPEG encode, Low-Res Motion Estimation, Computer Vision Proc.

---

## 3. RDI vs PIX paths (essential distinction)

```
 RDI (Raw Dump Interface):                PIX (Pixel/processed):
   CSID → IFE BUS → DDR  (untouched RAW)    CSID → IFE pipeline → DDR (demosaiced/YUV)
   - used for RAW capture, debugging,        - real-time preview/video with ISP
     sensors the SoC ISP can't process       - consumes IFE processing resources
   - minimal resources (IFE-lite ok)         - one PIX path per IFE typically
```

In **mainline qcom-camss**, RDI is the primary well-supported path (capture RAW, process
in userspace/libcamera). Production CamX uses PIX + offline BPS/IPE for full quality.
"Capture RAW via RDI" is the Qualcomm equivalent of the CSID-bypass debug technique
(Module 10 §10).

---

## 4. Mainline qcom-camss driver structure

`drivers/media/platform/qcom/camss/`:

```
 camss.c            top-level: probe, resources, media device, async notifier
 camss-csiphy*.c    CSIPHY subdev (per-SoC: 2ph, 3ph variants)
 camss-csid*.c      CSID subdev (gen1/gen2 variants)
 camss-vfe*.c       VFE/IFE subdev (many SoC-specific: 4-1, 4-7, 4-8, 17x, 480...)
 camss-ispif.c      (older SoCs) ISP interface mux between CSID and VFE
 camss-video.c      the capture video nodes (vb2 queues)
```

Each block is a `v4l2_subdev`; `camss.c` wires them into a media graph:

```
 /dev/media0:
   msm_csiphyN → msm_csidN → msm_vfeN_rdiM → msm_vfeN_videoM (capture node)
```

Probe flow (`camss_probe`):
```c
camss_init_subdevices(camss);          /* create csiphy/csid/vfe subdevs */
camss_register_entities(camss);        /* media entities + links */
v4l2_async_nf_init / parse_ports;      /* find sensors from DT ports (Module 7) */
v4l2_async_nf_register(&camss->notifier);  /* bind sensors when they appear */
/* on .complete: media_device_register + register video nodes */
```

---

## 5. CPAS, clocks, power domains, SMMU

Qualcomm camera resource management is centralized:

```
 CPAS (Camera Power And clock Subsystem):
   - votes AHB/AXI clock levels and DDR bandwidth (CAMNOC) per active use-case
   - sets up the camera SMMU context banks
   - gates the TITAN_TOP power domain (whole camera island)

 DT (Module 7):
   power-domains = <&camcc IFE_0_GDSC>, ..., <&camcc TITAN_TOP_GDSC>;
   clocks = <&camcc CAM_CC_*>;  clock-names = "cpas_ahb","camnoc_axi","vfe0",...;
   iommus = <&apps_smmu 0x808 0x0>;   /* SMMU stream IDs per context bank */
```

- **GDSCs** (Globally Distributed Switch Controllers) are the power domains for each
  IFE/Titan block; runtime PM gates them.
- **SMMU/CAMNOC**: every camera DMA goes through the SMMU (Module 14); the `iommus`
  stream IDs map each block to a context bank for isolation and scatter-gather.
- **Bandwidth voting**: CPAS votes CAMNOC bandwidth based on resolution/fps so DDR QoS
  is guaranteed (Module 16). Under-voting → IFE overflow / frame drops.

---

## 6. The production stack: CamX / CHI

```
 Android Camera HAL3 API
        │
 ┌──────▼─────────────────────────────────────────┐
 │  CamX  (Qualcomm camera HAL implementation)      │
 │   - Camera3 request/result model                 │
 │   - "Nodes" graph: Sensor, IFE, BPS, IPE, JPEG   │
 │   - Pipelines built per use-case (preview/snap)  │
 ├──────────────────────────────────────────────────┤
 │  CHI (Camera Hardware Interface) — OEM override   │
 │   - CHI-CDK: OEMs customize pipelines/features    │
 │   - Multi-camera, HDR, ZSL, custom nodes          │
 ├──────────────────────────────────────────────────┤
 │  ISP firmware (ICP) + 3A (AEC/AWB/AF) algorithms  │
 └──────┬───────────────────────────────────────────┘
        │  /dev/v4l-subdev*, /dev/cam_sync, cam_req_mgr ioctls
 ┌──────▼───────────────────────────────────────────┐
 │  Kernel cam_* drivers + Camera Request Manager     │
 └───────────────────────────────────────────────────┘
```

- **CamX** implements the Camera3 HAL: it receives capture requests, builds a **node
  graph** (Sensor→IFE→IPE→JPEG...), and schedules them.
- **CHI** is the OEM customization layer (CHI-CDK) where vendors add multi-camera logic,
  HDR modes, ZSL (zero-shutter-lag), and proprietary features.
- **3A** (AEC/AWB/AF) algorithms run as CamX components feeding sensor + IFE settings.

---

## 7. Camera Request Manager (CRM)

The downstream kernel's heart is `cam_req_mgr`:

```
 Userspace (CamX) submits a "request" (frame N) referencing:
   - sensor settings (exposure/gain) for frame N
   - IFE config for frame N
   - output buffers for frame N
        │
 cam_req_mgr synchronizes all hardware devices (sensor, CSID, IFE, ICP) so that
 each applies its part of request N on the SAME frame, using SOF events to
 schedule register writes ("apply settings") at the right time.
        │
 On frame done, results + buffers are returned to CamX, which builds the
 Camera3 result for that request.
```

This is Qualcomm's hardware-synchronized version of the **Request API** concept (Module
6) — but implemented as a custom kernel framework (`drivers/media/platform/qcom/camss`
is mainline; `cam_req_mgr` + `cam_*` is the downstream techpack). It guarantees per-frame
atomicity across many IPs with deep pipelines.

---

## 8. End-to-end streaming flow (mainline RDI capture)

```
 1. DT: camss node + sensor endpoints (Module 7) → sensors bind via async
 2. media-ctl: enable links sensor→csiphy→csid→vfe_rdi→video; set pad formats
 3. v4l2-ctl STREAMON on /dev/videoN
 4. camss-video start_streaming:
      - CPAS votes clocks + CAMNOC bandwidth, enables GDSCs (runtime PM)
      - program VFE BUS write-master (DMA dst = vb2 buffer, SMMU-mapped)
      - s_stream(1) down the chain: vfe → csid (VC/DT, RDI route) → csiphy
        (lanes + settle from sensor link_freq) → sensor (mode + streaming)
 5. Sensor emits frames → CSIPHY → CSID (CRC check) → IFE BUS → DDR
 6. VFE BUS "frame done" IRQ → vb2_buffer_done → userspace DQBUF
 7. SOF IRQ → program next buffer into write-master (ping-pong)
```

The **ping-pong** double-buffering of the write-master (two buffer addresses swapped per
frame) is how Qualcomm avoids tearing — analogous to vb2's queue management (Module 13).

---

## 9. Debugging Qualcomm camera

```bash
# Mainline qcom-camss
media-ctl -p -d /dev/media0                 # CAMSS topology
media-ctl -d /dev/media0 -l '"msm_csiphy0":1->"msm_csid0":0[1]'  # route
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=60          # RDI capture
dmesg | grep -iE "camss|csiphy|csid|vfe|cpas|overflow|sof|buf_done"

# CSID/VFE error counters via debugfs (where exposed)
ls /sys/kernel/debug/camss* 2>/dev/null

# Downstream (techpack) — much richer tracing
cat /sys/kernel/debug/camera/* 2>/dev/null
echo 0x... > /sys/module/cam_debug_util/parameters/...   # cam debug levels
# CamX logs: setprop persist.vendor.camera.logInfoMask ... ; logcat
```

Symptom → cause:
```
 No frames (mainline)            → link not enabled (media-ctl -l), sensor link_freq
                                    mismatch → CSIPHY settle wrong (Module 8)
 VFE/IFE overflow                → CPAS bandwidth under-vote (CAMNOC), DDR QoS (Mod 16)
 SMMU fault (context bank)       → wrong iommus stream IDs, unmapped buffer (Mod 14)
 GDSC/clock errors at stream     → power-domain/clock names wrong in DT
 CamX pipeline fails (downstream)→ CHI node graph / firmware (ICP) issue; check logcat
```

---

## 10. Qualcomm naming cheat-sheet

```
 CAMSS      Camera Subsystem (the whole island)
 Spectra    Marketing name for the Qualcomm ISP (IFE+BPS+IPE)
 CSIPHY     CSI D/C-PHY receiver
 CSID       CSI Decoder (VC/DT demux, CRC)
 IFE/VFE    Image/Video Front End — real-time ISP + write-masters
 IFE-lite   RDI-only front end (cheap RAW streams)
 RDI        Raw Dump Interface (RAW to DDR, bypass processing)
 PIX        Processed pixel path (through IFE ISP)
 CPAS       Camera Power, AHB/AXI clock & Bandwidth Subsystem
 CAMNOC     Camera Network-On-Chip (bus to DDR)
 ICP        Image Control Processor (DSP running ISP firmware)
 BPS        Bayer Processing Segment (offline RAW-domain)
 IPE        Image Processing Engine (offline YUV-domain)
 LRME/CVP   Low-Res Motion Est / Computer Vision Processor
 CamX/CHI   Userspace HAL3 + OEM customization layer
 CRM        Camera Request Manager (per-frame HW sync, downstream)
```

---

## 11. Interview Q&A

**Q1. Difference between RDI and PIX paths in CAMSS?**
RDI (Raw Dump Interface) writes untouched RAW from CSID straight to DDR, bypassing ISP
processing — used for RAW capture, debugging, and sensors the SoC ISP can't process,
with minimal resources (IFE-lite). PIX routes data through the IFE/VFE pipeline
(demosaic/scale/stats) producing processed YUV in real time, consuming IFE resources.

**Q2. What is CPAS and why is it central?**
The Camera Power, Clock & Bandwidth Subsystem. It votes AHB/AXI clock levels and CAMNOC
(DDR) bandwidth per active use-case, gates the camera power domain, and sets up the SMMU
context banks. Under-voting bandwidth causes IFE overflow/frame drops, so CPAS is the
nexus of camera power and QoS.

**Q3. Where does 3A run in the Qualcomm production stack?**
In the CamX/CHI userspace stack and ISP firmware (ICP), not the kernel. AEC/AWB/AF are
CamX components; the kernel (cam_* drivers + Camera Request Manager) synchronizes and
applies their results per frame. It's a Pattern-B firmware ISP (Module 10).

**Q4. What problem does the Camera Request Manager solve?**
Per-frame atomic configuration across many deeply-pipelined IPs (sensor, CSID, IFE, ICP).
It ensures all devices apply their portion of request N on the same frame, scheduling
register writes at SOF events — Qualcomm's hardware-synchronized analog of the V4L2
Request API.

**Q5. IFE reports overflow during high-res capture. First suspects?**
DDR bandwidth/QoS: CPAS is under-voting CAMNOC bandwidth for the resolution/fps, so the
write-master can't drain and the IFE input overflows. Check bandwidth votes, CAMNOC QoS,
and whether other clients are starving DDR (Module 16). Also verify clocks aren't capped.

**Q6. You get an SMMU context-bank fault from a camera buffer. Cause?**
The DMA accessed an unmapped/incorrectly-mapped address: wrong `iommus` stream IDs in
DT, a buffer not mapped into the camera context bank, or a stale/over-run write-master
address. The SMMU isolates camera DMA per context bank (Module 14); a fault means the
device touched memory outside its mapping.

**Q7. Mainline qcom-camss vs the downstream techpack — what's the practical difference?**
Mainline supports CSIPHY/CSID/VFE with mostly RDI (RAW) capture and basic PIX, paired
with libcamera for limited 3A — great for bring-up and RAW. The downstream techpack adds
the full Spectra ISP (BPS/IPE), ICP firmware, CamX/CHI HAL3, multi-camera, HDR, ZSL, and
the Camera Request Manager — what actually ships in phones.

**Q8. What are BPS and IPE and when are they used?**
Offline (non-real-time) Spectra ISP engines driven by ICP firmware: BPS (Bayer
Processing Segment) does RAW-domain work (HDR merge, demosaic, denoise) and IPE (Image
Processing Engine) does YUV-domain work (NR, sharpening, color). They process captured
RAW from DDR for high-quality snapshots/ZSL, separate from the real-time IFE preview path.

---

### Key takeaways
- CAMSS datapath: CSIPHY → CSID → (RDI raw | PIX via IFE/VFE) → BUS write-master → DDR;
  offline BPS/IPE via ICP for snapshots.
- Mainline `qcom-camss` = CSIPHY/CSID/VFE + RDI capture + libcamera; downstream
  CamX/CHI + cam_req_mgr + ICP firmware = full production Spectra stack.
- CPAS centralizes clocks, CAMNOC bandwidth voting, power domains (GDSCs), and SMMU;
  under-voting → overflow.
- The Camera Request Manager gives per-frame atomicity across deep pipelines, the
  Qualcomm analog of the V4L2 Request API.
