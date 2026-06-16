# Module 17 — Android Camera Stack

> **Scope:** The full Android camera stack from app to kernel, and how it sits on top of
> the V4L2/media kernel layers you've learned. Covers CameraX / Camera2 APIs, the
> framework Camera Service, the Camera HAL3 interface, the request/result model, vendor
> HALs (Qualcomm CamX, others), buffer flow via gralloc/dma-heap, and how a HAL3 capture
> request ultimately becomes V4L2 ioctls and ISP firmware commands. This connects the
> kernel (Modules 5-14) to the product. Camera HAL3 is a frequent senior-interview topic.

---

## 1. The whole stack at a glance

```
 ┌──────────────────────────────────────────────────────────┐
 │  APP   (uses CameraX or Camera2 directly)                  │  Java/Kotlin
 ├──────────────────────────────────────────────────────────┤
 │  CameraX (Jetpack)  → wraps Camera2 with lifecycle/use-cases│
 │  Camera2 API (android.hardware.camera2)                     │
 ├──────────────────────────────────────────────────────────┤
 │  Camera Framework (Java)  CameraManager/Device/CaptureSession│
 ├──────────────────────────────────────────────────────────┤
 │  Camera Service (native, C++)  cameraserver process         │  binder
 ├──────────────────────────────────────────────────────────┤
 │  Camera HAL3 interface (AIDL/HIDL)  ICameraDevice            │  HAL boundary
 ├──────────────────────────────────────────────────────────┤
 │  Vendor HAL (Qualcomm CamX/CHI, MTK, Samsung, Google GCA)    │  3A, ISP tuning
 ├──────────────────────────────────────────────────────────┤
 │  Kernel: V4L2 / media / cam_req_mgr / ISP firmware (ICP/RCE) │  what you write
 └──────────────────────────────────────────────────────────┘
```

Everything above the HAL boundary is Android framework; everything below is **your**
kernel + vendor driver world. The HAL3 contract is where they meet.

---

## 2. Camera2 and CameraX (the app APIs)

```
 Camera2 (low-level, explicit):
   - CameraManager.openCamera() → CameraDevice
   - createCaptureSession(surfaces) → CameraCaptureSession
   - CaptureRequest.Builder: set controls (exposure, AF mode, etc.), add Surfaces
   - session.capture(request) / setRepeatingRequest(request)
   - results delivered as CaptureResult (per frame)

 CameraX (high-level, Jetpack):
   - Use-cases: Preview, ImageCapture, ImageAnalysis, VideoCapture
   - Lifecycle-aware, handles device quirks, built ON TOP of Camera2
   - What most modern apps use; Camera2 is the explicit fallback
```

Both are **request-based**: the app submits capture requests with per-frame settings and
receives per-frame results — the same model as the kernel V4L2 Request API (Module 6) and
the vendor request managers (Modules 11/12). This symmetry is intentional and is the key
insight for understanding the stack.

---

## 3. The Camera Service (cameraserver)

The native `cameraserver` process:
- Owns the HAL connection; enforces permissions and per-app access/arbitration.
- Translates framework `CaptureRequest`s into HAL `capture requests`.
- Manages output **Surfaces** (preview, video, still) and their buffers (gralloc).
- Routes per-frame `CaptureResult`s (metadata) back to the app.

```
 App ──binder──► CameraService ──HAL3──► Vendor HAL ──► kernel
     ◄──results──            ◄──results──            ◄── frames/metadata
```

It's the trust + arbitration boundary: multiple apps, multiple cameras, permissions, and
buffer ownership are managed here.

---

## 4. Camera HAL3 — the contract you implement against

HAL3 (the "camera3" model, now expressed in AIDL; previously HIDL) is the heart. Its
model:

```
 ┌────────────── HAL3 streaming model ──────────────┐
 │ configure_streams(stream_config)                  │  set up output streams
 │   - each stream: width×height, format, usage      │    (preview/video/still/raw)
 │                                                    │
 │ process_capture_request(request)                  │  ONE request per frame:
 │   - settings (metadata: exposure, AF, AWB, ...)    │    in-flight, pipelined
 │   - output_buffers[] (one per active stream)       │
 │   - input_buffer (for reprocessing, ZSL)           │
 │                                                    │
 │ process_capture_result(result)  ◄── callback       │  per-frame result:
 │   - result metadata (actual exposure, timestamps)  │    partial results allowed
 │   - filled output buffers + release fences          │
 └────────────────────────────────────────────────────┘
```

Key HAL3 properties (classic interview material):
- **Per-frame request/result**: every frame is an explicit request with its own settings
  and its own result metadata. No hidden global state.
- **Pipelined / in-flight**: many requests are outstanding simultaneously (deep pipeline,
  Module 4 §7 latency). The HAL must keep the pipeline full.
- **Fences**: buffers carry acquire/release fences (sync_file / dma_fence) so producers
  and consumers synchronize without blocking the CPU (Module 14).
- **Static metadata**: the HAL advertises capabilities (supported sizes, formats, control
  ranges, hardware level: LEGACY/LIMITED/FULL/LEVEL_3) the framework queries once.
- **Reprocessing**: an output frame (e.g. RAW/YUV held in a ZSL ring) can be fed back as
  an input for high-quality stills (maps to Qualcomm BPS/IPE offline, Module 11).

---

## 5. The request → kernel translation

How a single HAL3 request becomes kernel activity (Qualcomm example):

```
 App: CaptureRequest{ exposure=8ms, AF=auto, output: preview+still }
   │  binder
 CameraService → HAL3 process_capture_request(req N)
   │
 CamX: builds/uses a pipeline (Sensor→IFE→IPE→JPEG nodes), schedules request N
   │
 CHI: OEM customization (multi-cam, HDR, ZSL) may rewrite the node graph
   │
 cam_req_mgr (kernel): synchronizes sensor + CSID + IFE + ICP to apply req N's
   settings on the SAME frame (Module 11 §7), using SOF events
   │
 ├─ sensor subdev: V4L2 controls exposure/gain (Module 9) for frame N
 ├─ CSID/CSIPHY: receive frame N (Module 8)
 ├─ IFE: process → write-master DMA to a gralloc/dma-heap buffer (Module 14)
 └─ ICP firmware: offline IPE/BPS for the still (Module 11)
   │
 frame done → buffers filled + result metadata → process_capture_result(N)
   │  back up through CameraService → app's CaptureResult + Surface frames
```

The settings the app set in `CaptureRequest` ultimately become **V4L2 control writes**
and **ISP firmware commands**, applied atomically to frame N. That's the through-line from
Android API to the registers you program.

---

## 6. Buffers: gralloc, dma-heap, Surfaces, fences

```
 - Output targets are Android Surfaces backed by gralloc buffers (BufferQueue).
 - gralloc allocates via dma-heap (Module 14) → dma-buf fds with usage flags
   (CAMERA_WRITE, GPU_TEXTURE, VIDEO_ENCODER, COMPOSER_OVERLAY...).
 - The HAL receives dma-buf handles, maps them into the camera SMMU context, and the
   IFE write-master DMAs directly into them → ZERO COPY to GPU/encoder/display.
 - Fences (acquire fence: wait before writing; release fence: signal when done) keep
   producer/consumer in sync without CPU stalls.
```

```
 Camera (HAL) ──writes──► gralloc/dma-buf ──reads──► SurfaceFlinger (display)
                                │                  └─► MediaCodec (encoder)
                          one buffer, many consumers, zero copy, fence-synchronized
```

The buffer usage flags drive which SMMU contexts and which hardware (GPU/encoder/display)
can access the buffer — a direct application of Module 14.

---

## 7. Vendor HAL variants

```
 Qualcomm: CamX + CHI (CHI-CDK) — covered in Module 11. Implements HAL3 on CAMSS/Spectra.
 MediaTek: their own ImgSensor/ISP HAL.
 Samsung:  Exynos camera HAL.
 Google:   GCA (Google Camera) + the "Google Camera HAL" / HAL3 reference; Pixels add
           computational photography on top (HDR+, Night Sight) above the vendor HAL.
 Generic/AOSP: the v4l2 camera HAL / external camera HAL (USB UVC via V4L2).
```

The **external/USB camera HAL** is notable for kernel engineers: it sits directly on
**V4L2** (`/dev/video*` from the `uvcvideo` driver), bypassing a vendor ISP — the most
direct "Android HAL3 → V4L2" path and a good mental model.

---

## 8. Where 3A and image quality live

```
 Android framework: sets 3A MODES (AE/AWB/AF on/off, regions, locks) per request,
                    reads 3A STATE in results — but does NOT run the algorithms.
 Vendor HAL/firmware: actually runs AE/AWB/AF and ISP tuning (CamX/ICP, Module 4/11).
 Kernel: applies the resulting sensor exposure/gain + ISP config per frame.
```

So the Android API exposes 3A *control and reporting*, while the *intelligence* is in the
vendor HAL + firmware, and the *application* of results is in the kernel — three layers,
one loop. Pixel "computational photography" (HDR+, Night Sight) adds another userspace
layer above the HAL that captures bursts and merges them.

---

## 9. Debugging the Android camera stack

```bash
# Framework / service logs
logcat | grep -iE "Camera|CameraService|Camera2|CameraX|Hal"

# Vendor (Qualcomm CamX) verbose logging
setprop persist.vendor.camera.logInfoMask 0x...      # CamX log masks
setprop persist.vendor.camera.logCoreCfgMask 0x...
logcat | grep -iE "CamX|CHI|camxhal"

# dumpsys: full camera service state, sessions, streams
dumpsys media.camera

# Down at the kernel (same as Modules 9-15)
dmesg | grep -iE "camss|cam_|csid|ife|smmu|overflow"
cat /sys/kernel/debug/camera/* 2>/dev/null
media-ctl -p -d /dev/media0     # if the vendor exposes media nodes
```

```
 Triage: which layer?
   - App sees error/no frames but dumpsys shows configured streams → vendor HAL/kernel
   - HAL configures but process_capture_result never returns buffers → kernel pipeline
     (cam_req_mgr / IFE / SMMU fault) — drop to Module 11/14 debugging
   - Wrong image quality → vendor 3A/ISP tuning (HAL/firmware), not the kernel transport
   - Permission/access denied → CameraService layer
```

The kernel engineer's job usually starts when "the HAL configured streams but buffers
never come back" — that's a kernel pipeline/SMMU/bandwidth problem (Modules 11/14/16).

---

## 10. Interview Q&A

**Q1. Describe the Android camera HAL3 model.**
A per-frame request/result, pipelined model: the app/framework submits a capture request
with explicit settings (metadata) and one output buffer per configured stream;
`process_capture_request` is called per frame, many in-flight; the HAL returns
`process_capture_result` per frame with actual metadata and filled buffers carrying
release fences. The HAL advertises static metadata (capabilities, hardware level).

**Q2. How does a Camera2 CaptureRequest reach the registers you program?**
The framework sends it to CameraService → HAL3 `process_capture_request`. The vendor HAL
(e.g. CamX/CHI) schedules it on a pipeline; the kernel request manager (cam_req_mgr)
synchronizes sensor/CSID/IFE/ICP to apply request N atomically on one frame — translating
the request's settings into V4L2 control writes (exposure/gain) and ISP firmware commands.

**Q3. Where does 3A actually run in Android?**
Not in the framework. The framework sets 3A modes/regions/locks per request and reports 3A
state in results, but the AE/AWB/AF algorithms and ISP tuning run in the vendor HAL and
ISP firmware (CamX/ICP, Argus/RCE). The kernel applies the results per frame. It's a
three-layer split: control (framework), intelligence (HAL/firmware), application (kernel).

**Q4. How are buffers shared zero-copy from camera to display/encoder in Android?**
Output Surfaces are backed by gralloc buffers allocated from dma-heap as dma-buf fds with
usage flags. The HAL maps them into the camera SMMU context and the IFE DMAs frames
directly in; the same dma-buf is consumed by SurfaceFlinger/MediaCodec via their own SMMU
contexts, synchronized by acquire/release fences — no CPU copy (Module 14).

**Q5. The HAL configures streams but the app gets no frames. Whose problem is it?**
Likely the kernel pipeline: if `process_capture_request` is accepted but
`process_capture_result` never returns filled buffers, the frames aren't completing — a
cam_req_mgr/IFE issue, an SMMU context fault on the output buffer, or bandwidth overflow.
Drop to kernel debugging (dmesg SMMU/overflow, media-ctl, Modules 11/14/16).

**Q6. What is reprocessing / ZSL and how does it map to hardware?**
Zero-Shutter-Lag keeps a ring of recently captured frames (RAW/YUV) so a still "capture"
returns an already-buffered frame instantly. Reprocessing feeds such a frame back as the
input_buffer of a new request for high-quality processing — mapping to offline ISP engines
(Qualcomm BPS/IPE via ICP, Module 11) rather than the real-time preview path.

**Q7. Why is the request/result model consistent from Camera2 down to the kernel?**
Because deep camera pipelines have multi-frame latency; per-frame atomic settings are the
only correct way to guarantee "these settings + this buffer = this frame." Camera2's
CaptureRequest, HAL3's process_capture_request, the V4L2 Request API, and vendor request
managers all express the same per-frame atomicity at different layers.

**Q8. Where does the external/USB camera HAL sit and why is it instructive?**
It implements HAL3 directly on top of V4L2 (`uvcvideo` `/dev/video*`), with no vendor ISP.
It's the clearest example of "HAL3 request → V4L2 ioctls," showing the framework's
request/result model mapping straight onto QBUF/DQBUF/controls without a firmware black
box in between.

---

### Key takeaways
- The Android stack: App → CameraX/Camera2 → CameraService → HAL3 → vendor HAL → kernel
  V4L2/media/firmware; the HAL3 boundary is where Android meets your driver.
- HAL3 is per-frame request/result, deeply pipelined, fence-synchronized — the same
  atomic per-frame model as the V4L2 Request API and vendor request managers.
- 3A/image-quality lives in the vendor HAL + firmware; the framework only sets modes and
  reports state; the kernel applies results per frame.
- Buffers are gralloc/dma-heap dma-bufs shared zero-copy to GPU/encoder/display via SMMU
  contexts and fences; "HAL configured but no frames" is a kernel pipeline/SMMU/bandwidth
  bug.
