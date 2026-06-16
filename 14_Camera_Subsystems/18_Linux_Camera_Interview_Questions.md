# Module 18 — Linux Camera Interview Questions (Master Q&A Bank)

> **Scope:** A consolidated, categorized question bank for senior Linux camera
> driver / BSP interviews, drawing on Modules 1–17. Organized by topic with concise,
> interview-ready answers, plus real scenario/system-design questions at the end. Use
> this for final-stage prep; each section cross-references the deep-dive module.

> **How to use:** Read the question, answer aloud, then check. The scenario questions
> (§11–12) are where senior candidates are differentiated — practice walking the whole
> pipeline out loud.

---

## 1. Camera Fundamentals & Sensor (Modules 1, 3)

**1. Why is exposure programmed in lines, not microseconds?**
Integration is timed relative to the row-readout clock: `exposure_us = coarse_lines ×
line_length / pixel_clock`. The driver converts the user's microseconds to the sensor's
native line units.

**2. Derive frame rate from sensor timing registers.**
`fps = pixel_clock / (HTS × VTS)` where HTS = LINE_LENGTH_PCK and VTS =
FRM_LENGTH_LINES. Raise VTS (vertical blanking) to lower fps.

**3. What limits max exposure at a given fps?**
`exposure ≤ VTS × line_time`. Higher fps → smaller VTS → shorter max exposure; you must
drop fps for longer exposures.

**4. Analog vs digital gain — which preserves SNR?**
Analog gain amplifies before the ADC, lifting signal above the read/ADC noise floor →
better usable SNR. Digital gain multiplies post-ADC, scaling signal and noise equally →
no SNR gain. AE fills analog first.

**5. What is the Bayer pattern and why twice as much green?**
A CFA with 50% green, 25% red, 25% blue (RGGB/BGGR/...). Green is doubled because human
vision is most sensitive to green luminance.

**6. You crop the sensor window by one line — what breaks?**
The Bayer phase flips (e.g. RGGB→GBRG), so the advertised media bus code must change or
demosaic yields false color. Crop by even offsets to preserve phase.

**7. Rolling vs global shutter at the silicon level.**
Rolling = standard 4T pixel read row-by-row, each row time-shifted → motion skew. Global
= per-pixel charge storage (5T) latching all pixels simultaneously → no skew, larger
costlier pixel.

**8. What is MIPI-packed RAW10 and why does it matter?**
4 pixels packed into 5 bytes (no padding) on the CSI bus. It changes
bytesperline/sizeimage math; if the ISP unpacks to 16-bit in DDR the stride differs.
Wrong stride shears the image.

**9. How does binning help low light and what does it cost?**
2×2 binning sums four same-color pixels: signal ~4×, noise ~2× → SNR ~2× better, with
1/4 resolution and lower bandwidth.

**10. Why does enabling HDR sometimes show two frames per sensor frame at the CSID?**
Stagger/DOL HDR emits multiple exposures per frame on separate virtual channels /
interleaved lines; the CSID demuxes them into separate streams the ISP merges. Expected.

---

## 2. ISP & Image Processing (Module 4)

**11. List the ISP pipeline order.**
BLC → DPC → LSC → AWB → demosaic → CCM → gamma/tonemap → NR → sharpening → CSC →
scale/crop → NV12 out. Order is fixed by image math.

**12. Image has correct exposure but a green tint — sensor or ISP?**
ISP — AWB. A uniform color cast is wrong white-balance gains; correct exposure rules out
AE/sensor.

**13. Why is demosaic the most expensive ISP stage and its artifacts?**
It interpolates two missing colors per pixel at full resolution. Bad demosaic →
zippering on edges, false color/moiré on fine detail.

**14. What are ISP statistics and the kernel's role?**
Per-frame luma histograms, region color sums, AF contrast/phase the ISP DMAs to memory.
The kernel configures the stats engines, routes the DMA, and delivers stats to userspace
(metadata node); it doesn't run 3A in firmware-ISP designs.

**15. Why a 2–3 frame latency in the 3A loop?**
Stats from frame N are ready after N is processed, by which time N+1 is in flight; new
settings can only land on N+2. Hence the Request API for per-frame atomicity.

**16. Why output NV12 instead of RGB?**
NV12 (YUV 4:2:0 semi-planar) halves chroma data and is the native input for hardware
encoders and GPUs → zero-copy. RGB wastes bandwidth and isn't encoder-friendly.

**17. Contrast AF vs PDAF.**
Contrast AF sweeps the VCM maximizing high-frequency energy (slow, hunting). PDAF uses
phase-detection pixels to compute focus direction/magnitude in one shot (fast).

---

## 3. Media Controller & V4L2 (Modules 5, 6)

**18. Why was the Media Controller added when V4L2 existed?**
V4L2 modeled one monolithic device; modern pipelines are many independently-configurable
blocks with selectable routing. MC exposes a discoverable graph (entities/pads/links) for
topology inspection, routing, and format propagation/validation.

**19. Sub-device vs video node?**
Sub-device (`/dev/v4l-subdevN`) = internal processing block, configured via pad formats,
no buffers. Video node (`/dev/videoN`) = DMA endpoint owning the vb2 queue where
QBUF/DQBUF happen.

**20. STREAMON returns -EPIPE. Meaning?**
Pipeline validation failed: a sink pad format doesn't match the upstream source pad
(resolution or media bus code). Fix with consistent `media-ctl -V` pad formats end-to-end.

**21. Walk the V4L2 capture ioctl sequence.**
QUERYCAP → ENUM_FMT → S_FMT → REQBUFS → QUERYBUF+mmap → QBUF(all) → STREAMON →
loop(DQBUF→process→QBUF) → STREAMOFF.

**22. Pixel format vs media bus code?**
Pixel format (`V4L2_PIX_FMT_*`, fourcc) describes data in memory at the video node;
media bus code (`MEDIA_BUS_FMT_*`) describes data on the internal bus between subdev
pads. The driver maps between them.

**23. MMAP vs USERPTR vs DMABUF.**
MMAP: kernel allocates, userspace maps (common). USERPTR: userspace pointers (legacy,
IOMMU/cache-unfriendly). DMABUF: dma-buf fds shared zero-copy with GPU/encoder (modern).

**24. Why the Request API?**
To apply controls + a buffer atomically to a specific future frame. The 3A loop and HDR
need "this exposure + this gain + this buffer = frame N" despite pipeline latency.

**25. How are sensor subdevs bound to the CSI bridge given async probe order?**
V4L2 async + fwnode: the bridge registers a notifier listing expected remote endpoints
(from DT); sensors register themselves; when all appear, `.complete()` finalizes the
graph and registers video nodes.

**26. S_FMT returns a different size than requested — bug?**
No. V4L2 lets drivers adjust to the nearest supported format; userspace must read back the
returned `v4l2_format`.

---

## 4. MIPI CSI-2 (Module 8)

**27. Describe the CSI-2 long packet.**
Header (Data Identifier = VC + Data Type, Word Count, ECC) + payload (one line, WC bytes)
+ footer (CRC-16). ECC protects the header; CRC protects the payload.

**28. What is a virtual channel?**
A field in the Data Identifier multiplexing multiple logical streams onto one physical
link (two cameras over SerDes, or HDR exposures). The CSID demuxes by VC.

**29. D-PHY vs C-PHY.**
D-PHY: 2 wires/data-lane + separate clock lane, LP/HS, up to ~2.5 Gbps/lane. C-PHY: 3-wire
trio with embedded clock, 3-phase symbols (~2.28 bits/symbol), more throughput per wire.

**30. Intermittent CRC errors worsening at higher resolution. Cause?**
CSIPHY HS settle (T_HS_SETTLE) or deskew timing wrong for the link frequency. Higher rate
= tighter timing, so a marginal settle fails. Recompute settle from `link-frequency`.

**31. ECC vs CRC coverage.**
ECC (Hamming) on the header: corrects 1-bit, detects 2-bit header errors. CRC-16 on the
payload: detects (no correction) corrupted pixel data.

**32. How does the SoC know a frame started/ended?**
Frame Start (DT 0x00) and Frame End (DT 0x01) short packets. CSID raises SOF on FS,
completes the buffer on FE.

**33. CSID FIFO/buffer overflow indicates what?**
Downstream backpressure — ISP/DMA can't drain (usually DDR bandwidth/QoS), not a PHY
problem (which would be CRC).

**34. Compute link bandwidth for 1080p60 RAW10.**
`1920×1080×60×10 ≈ 1.24 Gbps` + ~10-20% overhead, divided by lane count for per-lane rate.

---

## 5. Device Tree (Module 7)

**35. Explain the OF graph binding for a camera link.**
`port`/`endpoint` nodes; each endpoint's `remote-endpoint` phandle points at the other
side. The kernel matches phandles to build the media graph. `data-lanes`, `clock-lanes`,
`link-frequencies`, `bus-type` describe MIPI config.

**36. Sensor probes but CSI CRC errors — which DT properties?**
`data-lanes` and `link-frequencies` (and `bus-type`) on both endpoints must match each
other and the sensor's PLL/lanes; a mismatch misconfigures CSIPHY settle → CRC.

**37. What causes a permanent `-EPROBE_DEFER`?**
A never-resolving dependency: wrong clock/regulator name so `devm_*_get` keeps failing, or
a bad `remote-endpoint` phandle so the async notifier never completes. Check
`/sys/kernel/debug/devices_deferred`.

**38. How do clocks/regulators in DT relate to the power sequence?**
DT declares resources; the driver acquires them (`clk_get`/`regulator_bulk_get`/
`gpiod_get`) and enables them in the datasheet order/timing during power-on/runtime-PM
resume.

**39. GPIO polarity bug — how does it manifest?**
A wrong `GPIO_ACTIVE_LOW/HIGH` on `reset-gpios` holds the sensor in reset (or releases at
the wrong time), so chip-ID reads fail and the sensor never probes.

---

## 6. Sensor Driver (Module 9)

**40. Most important check in probe()?**
Reading the chip-ID after power-on — validates power sequence, clock, reset polarity, and
I2C addressing before registering the subdev.

**41. Why guard `s_ctrl` with `pm_runtime_get_if_in_use()`?**
Controls can be set while the sensor is powered off (values cached); only write registers
when powered. The guard returns 0 (skip HW write) if suspended, avoiding I2C to a dead
device.

**42. Order of operations in `s_stream(1)`.**
Power on → write mode register table → apply cached controls
(`__v4l2_ctrl_handler_setup`) → set the streaming/MODE_SELECT bit. Setting streaming
before the mode table = garbage frames.

**43. How does changing VBLANK affect exposure?**
VBLANK sets VTS; exposure ≤ VTS − margin. On VBLANK change, recompute exposure max and
`__v4l2_ctrl_modify_range()` the exposure control.

**44. How does the CSIPHY learn the link frequency?**
From the sensor's read-only `V4L2_CID_LINK_FREQ` int-menu control, read through the media
graph; the CSIPHY computes D-PHY settle from it.

**45. Runtime PM vs system suspend in a sensor driver.**
Runtime PM powers on/off per use (`SET_RUNTIME_PM_OPS`). System suspend stops streaming +
powers off on `.suspend`, restarts on `.resume`. Separate callback sets; both required.

**46. Why regmap_i2c over raw i2c_transfer?**
Abstracts register width/endianness, provides read/write/bulk helpers, optional caching +
debugfs register dumps, less boilerplate.

---

## 7. videobuf2 & Memory (Modules 13, 14)

**47. What does vb2 provide?**
Buffer allocation, the three memory models, DMA mapping + cache coherency, the buffer
state machine, and QBUF/DQBUF/STREAMON plumbing. The driver implements `queue_setup`,
`buf_prepare/queue`, `start/stop_streaming`, and IRQ completion.

**48. Why must stop_streaming return every buffer?**
Userspace blocks in DQBUF and vb2 can't free/re-init the queue while the driver owns
buffers. Not returning them → DQBUF hangs, STREAMOFF/close deadlocks.

**49. dma-contig vs dma-sg vs vmalloc backends.**
dma-contig: physically contiguous (no IOMMU, may need CMA). dma-sg: scatter-gather pages +
SG table for IOMMU/SG DMA. vmalloc: CPU-only, software/test drivers.

**50. Garbage/stale image but correct DMA address. Cause?**
Cache coherency — CPU cache not invalidated after the device DMA, so the CPU reads stale
lines. vb2 normally handles it; perf hacks that bypass sync reintroduce it.

**51. Why does camera sit behind an SMMU on ARM64?**
Scatter-gather (non-contiguous pages look contiguous), isolation (context banks/stream
IDs; bad DMA faults instead of corrupting), and wide addressing (32-bit DMA reaches >4 GB
via IOVA).

**52. "arm-smmu Unhandled context fault, iova=..., cb=7" — debug it.**
The DMA hit an unmapped IOVA. Use the context bank + DT `iommus` stream IDs to find the
faulting block; verify the buffer was mapped into that context and the write-master got a
valid IOVA (not stale/over-run).

**53. dma-buf and dma-heap — what and why.**
dma-buf shares one buffer's pages across devices via an fd with fences → zero-copy
camera→encoder→display. dma-heap is the mainline ION replacement allocating dma-buf-backed
buffers via `/dev/dma_heap/*`.

**54. When do you need CMA and its downside?**
When hardware needs contiguous memory (no SMMU). CMA is a reclaimable contiguous pool;
under fragmentation/pressure allocation can fail/stall (`-ENOMEM`). With an SMMU, dma-sg
avoids CMA.

---

## 8. SoC Architecture — Qualcomm & NVIDIA (Modules 11, 12)

**55. RDI vs PIX path in CAMSS.**
RDI writes untouched RAW from CSID to DDR (bypass ISP) — RAW capture/debug, minimal
resources. PIX routes through the IFE/VFE pipeline producing processed YUV in real time.

**56. What is CPAS and why central?**
Camera Power, Clock & Bandwidth Subsystem — votes AHB/AXI clocks + CAMNOC (DDR) bandwidth
per use-case, gates the power domain, sets up SMMU context banks. Under-voting → IFE
overflow/drops.

**57. Where does 3A run in Qualcomm production?**
CamX/CHI userspace + ICP firmware (Pattern B). The kernel (cam_* + Camera Request Manager)
synchronizes and applies results per frame.

**58. What does the Camera Request Manager solve?**
Per-frame atomic configuration across deeply-pipelined IPs (sensor/CSID/IFE/ICP), applying
each device's portion of request N on the same frame via SOF scheduling — Qualcomm's analog
of the V4L2 Request API.

**59. Describe the NVIDIA Tegra datapath.**
Sensor → NVCSI → VI (DMA to DRAM) → ISP (RCE firmware, libargus). Mainline `tegra-video`
does NVCSI+VI RAW capture; ISP/3A are L4T/firmware.

**60. host1x and syncpoints?**
host1x is Tegra's command-channel engine all multimedia blocks submit to; syncpoints are
hardware counters incremented on completion, used as fences. Camera completion is a
syncpoint increment — different from Qualcomm's IRQ+ping-pong.

**61. Qualcomm vs NVIDIA scheduling model.**
Qualcomm: IRQ + write-master ping-pong + Camera Request Manager. NVIDIA: host1x channels +
syncpoint fences + libargus/RCE. Both firmware ISPs; different kernel scheduling
primitives.

**62. Where do Jetson sensor modes live vs mainline?**
Mainline: C tables in the driver. Jetson/L4T: also in device tree (`mode0 { active_w,
line_length, pix_clk_hz, ... }`) + `tegra-camera-platform`, consumed by libargus/ISP
tuning.

---

## 9. Android Stack (Module 17)

**63. Describe the HAL3 model.**
Per-frame request/result, pipelined: each request has settings + one buffer per stream;
`process_capture_request` per frame, many in-flight; `process_capture_result` returns
metadata + filled buffers with fences. HAL advertises static metadata/hardware level.

**64. How does a CaptureRequest reach registers?**
App → CameraService → HAL3 `process_capture_request` → vendor HAL (CamX/CHI) schedules →
kernel request manager synchronizes sensor/CSID/IFE/ICP to apply it atomically on frame N
→ V4L2 control writes + ISP firmware commands.

**65. Where does 3A run in Android?**
Vendor HAL + ISP firmware, not the framework. The framework sets 3A modes/regions/locks
and reports state; the kernel applies results per frame.

**66. Zero-copy buffer flow in Android.**
Surfaces backed by gralloc/dma-heap dma-bufs with usage flags; the HAL maps them into the
camera SMMU context, the IFE DMAs frames in, and SurfaceFlinger/MediaCodec consume the
same dma-buf via their SMMU contexts, fence-synchronized — no CPU copy.

**67. HAL configured streams but app gets no frames. Whose problem?**
Likely the kernel pipeline: requests accepted but results never return filled buffers →
cam_req_mgr/IFE issue, SMMU fault on the output buffer, or bandwidth overflow. Drop to
kernel debugging.

---

## 10. Debugging & Performance (Modules 15, 16)

**68. Camera shows black screen — triage.**
Isolate the layer: confirm device nodes exist (probe/DT); `media-ctl -p` (links/formats);
sensor chip-ID + `s_stream`; SOF interrupts + CSID CRC/overflow; capture RAW via RDI to
split sensor/CSI from ISP.

**69. Enable driver debug without rebuilding.**
Dynamic debug: `echo 'module <name> +p' > /sys/kernel/debug/dynamic_debug/control`, watch
`dmesg -w`.

**70. Single most useful "image looks wrong" isolation technique.**
Capture RAW from the CSID bypass/RDI. Good RAW + bad YUV = ISP config; bad RAW = sensor/CSI.

**71. CSID CRC counter climbing — cause and next step.**
PHY/link integrity: CSIPHY settle wrong for link freq, or lane/link-freq mismatch.
Recompute settle, verify DT endpoints match, then MIPI analyzer/scope.

**72. Works at 1080p30, drops at 1080p60 — where to look?**
Bandwidth/QoS: doubled CSI + DDR bandwidth. Check the bandwidth vote covers the rate, DDR
clock isn't scaled down, and overflow counters. Usually under-voted bandwidth.

**73. Estimate DDR bandwidth for 4K30 NV12 with TNR + encode.**
Frame ≈ 12 MB; passes ≈ IFE write + TNR read + ISP write + encoder read + display read ≈ 5;
12 MB × 30 × 5 ≈ 1.8 GB/s for camera alone, plus other clients.

**74. Records fine for minutes then drops frames. Diagnosis.**
Thermal throttling — sustained load heats the SoC, governor lowers ISP/DDR clocks → drops.
Correlate with thermal-zone temps + clock frequencies over time.

**75. Glass-to-glass latency sources and reductions.**
Exposure + readout + ISP depth (shadow latch) + buffer count + encoder/compositor +
display vsync (3-6 frames). Reduce buffers, shorten exposure, lighten ISP for low-latency
modes, ZSL, bypass encoder for preview.

---

## 11. Real scenario questions (walk the whole pipeline)

**S1. "New sensor on a new board won't probe. Walk me through bring-up."**
Check power rails (order/voltage), XCLK present and correct (scope it), reset polarity and
timing, I2C/CCI address + ACK, then chip-ID read. In DT verify
clocks/regulators/reset-gpios names and the OF graph endpoints. `dmesg` for EPROBE_DEFER;
`/sys/kernel/debug/devices_deferred`. Most failures: power sequence or missing/wrong XCLK,
reset polarity, or a DT resource name.

**S2. "Sensor streams but image is garbled/striped at high res only."**
MIPI link mismatch: lane count or link frequency differs between sensor and CSIPHY, or
CSIPHY settle timing is wrong for the high data rate. Check CSID CRC/ECC counters; verify
`data-lanes`/`link-frequencies` match on both DT endpoints and the advertised
`V4L2_CID_LINK_FREQ` for the high-res mode; recompute settle. Confirm with a MIPI analyzer.

**S3. "DQBUF hangs, no frames arrive."**
`media-ctl -p` (links/formats); dmesg for SOF + CRC/overflow. No SOF → sensor not streaming
or CSIPHY not locking. SOF + rising CRC → settle/link-freq. SOF, no CRC, buffers not done →
write-master DMA address not programmed or IRQ not firing. Trace `s_stream` chain with
dynamic debug; if MIPI is silent, scope the lanes.

**S4. "Image colors are wrong (green/pink cast)."**
Capture RAW via RDI: if RAW is correct, it's the ISP — AWB gains or CCM (color cast), or
wrong Bayer phase/bus code (false color). If RAW is wrong, it's sensor format/bus code or a
crop-induced Bayer-phase flip. Brightness issues = AE/exposure not applied.

**S5. "SMMU context fault during streaming."**
Decode the fault: iova + context bank → DT `iommus` stream ID → which block. Verify the
output buffer was mapped into that context (dma-buf attach/map), the write-master got a
valid mapped IOVA (not stale/over-run), and stream IDs match hardware. Often a buffer
lifecycle or over-run bug.

**S6. "Frames drop only when the GPU/display is busy."**
DDR QoS contention: camera bandwidth vote insufficient or a higher-priority client starves
the bus; DDR clock may be scaled by devfreq. Raise the camera bandwidth vote / mark it
latency-critical/ISO, pin DDR clock during capture, and reduce passes via zero-copy.

---

## 12. System-design questions

**D1. Design the camera pipeline for a 4-camera 1080p30 surround-view ADAS system.**
Budget CSI lanes and CSIPHY instances (4 sensors, possibly via GMSL SerDes aggregating onto
fewer CSI ports using virtual channels). Frame-sync the sensors (hardware FSIN) for correct
stitching. Use RDI for raw capture if processing is done by a CV accelerator, or per-camera
IFE if real-time ISP is needed. DDR budget = 4 × frame_size × 30 × passes + other clients;
vote CAMNOC/EMC bandwidth accordingly. Global-shutter sensors if motion skew is
unacceptable. SMMU contexts per camera for isolation. Plan thermal headroom for sustained
operation.

**D2. Design zero-copy capture→encode→stream for a 4K60 IP camera.**
Allocate frame buffers from dma-heap as dma-bufs; camera IFE write-master DMAs directly in
(SMMU-mapped). Share the same dma-buf to the hardware encoder (no CPU copy), fence-
synchronized. Budget DDR for IFE write + encoder read passes; ensure bandwidth vote covers
4K60. Use NV12. Minimize buffer count for latency while preventing drops. Avoid CPU touch of
frames (skip cache maintenance). Pin DDR/ISP clocks; monitor thermal.

**D3. A sensor outputs 108MP — the single ISP can't keep up. Architect it.**
Sensor binning/crop to reduce real-time preview pixel rate; for full-res capture, split the
frame across multiple IFEs (dual/triple-IFE stripes) and stitch, or capture RAW (RDI) and
process offline via BPS/IPE (Qualcomm) asynchronously for stills. Keep the real-time path
within one IFE's throughput; do heavy processing off the critical path. ZSL ring for instant
capture.

**D4. Explain the end-to-end per-frame atomicity story across all layers.**
Camera2 CaptureRequest → HAL3 process_capture_request → vendor HAL pipeline → kernel request
manager (cam_req_mgr / V4L2 Request API) → sensor V4L2 controls + ISP config applied on the
same frame N at SOF → result returned per frame. Every layer expresses the same per-frame
request/result model because deep pipeline latency makes per-frame atomicity the only correct
way to bind settings to a specific frame.

---

### How to stand out
- Always **isolate the layer** before proposing a fix (sensor → CSI → ISP → DMA → node).
- Quote the **two key formulas**: `fps = pixclk/(HTS×VTS)` and CSI/DDR bandwidth =
  `w×h×fps×bpp` (× passes for DDR).
- Know the **two signature bugs**: CSIPHY settle (CRC errors) and SMMU context fault
  (decode iova + context bank).
- Distinguish **PHY problems (CRC)** from **bandwidth problems (overflow)** from **thermal
  (drops after minutes)**.
- Tie every Android/HAL3 question back to **per-frame request/result atomicity** and
  **zero-copy dma-buf**.
