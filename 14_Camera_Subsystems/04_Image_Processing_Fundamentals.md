# Module 4 — Image Processing Fundamentals (the ISP)

> **Scope:** What the Image Signal Processor (ISP) does to turn RAW Bayer into a
> viewable YUV/RGB image. Covers the full ISP pipeline stage-by-stage: black level,
> lens shading, demosaic, white balance, color correction, gamma, tone mapping, noise
> reduction, sharpening, plus the 3A statistics (AE/AWB/AF) loop. Written so you can
> map each algorithmic stage to an ISP hardware block and its driver/stats interface
> (Modules 10–12).

---

## 1. Why a kernel engineer needs the ISP picture

The sensor gives you RAW Bayer — it is **not** a viewable image. The ISP transforms
it. As a driver engineer you generally do **not** write the math, but you must:

- Configure ISP **hardware blocks** (enable/bypass, parameters) from the driver.
- Wire **statistics DMA** so the 3A algorithms (in userspace/HAL) get histograms.
- Apply 3A results (exposure, gain, WB gains, focus, LSC) back to sensor + ISP.
- Debug "image looks wrong" by knowing *which stage* produces which artifact.

```
3A loop (the control system you plumb through the kernel):

  ISP stats DMA ──► userspace 3A (AE/AWB/AF) ──► new settings
        ▲                                              │
        │                                              ▼
   RAW frame ◄── sensor (exposure/gain) ◄── apply via V4L2 controls / ISP regs
```

---

## 2. The ISP pipeline (canonical order)

```
 RAW Bayer
   │
   ▼
[1] Black Level Correction (BLC)      subtract optical-black pedestal
   │
   ▼
[2] Defect Pixel Correction (DPC)     fix dead/hot pixels
   │
   ▼
[3] Lens Shading Correction (LSC)     compensate vignetting (corners darker)
   │
   ▼
[4] White Balance (AWB gains)         per-channel R/G/B gain
   │
   ▼
[5] Demosaic (CFA interpolation)      Bayer → full RGB per pixel
   │
   ▼
[6] Color Correction Matrix (CCM)     3×3 matrix → correct color reproduction
   │
   ▼
[7] Gamma / Tone Mapping              compress dynamic range, perceptual encoding
   │
   ▼
[8] Noise Reduction (2D/3D/TNR)       spatial + temporal denoise
   │
   ▼
[9] Sharpening / Edge Enhancement
   │
   ▼
[10] Color Space Conversion (RGB→YUV) + Scaler + Crop
   │
   ▼
 YUV/NV12 frame → DMA → DDR
```

Different SoCs split this across blocks differently, but the *order* is fundamentally
fixed by image math. Knowing the order lets you localize artifacts.

---

## 3. Stage-by-stage

### 3.1 Black Level Correction (BLC)
Even in total darkness the ADC outputs a small pedestal (optical black + dark
current). BLC subtracts it so true black is code 0. Wrong BLC → milky/washed blacks or
clipped shadows.

### 3.2 Defect Pixel Correction (DPC)
Sensors have a few dead (always black) or hot (always bright) pixels. DPC detects
outliers vs neighbors and replaces them. Static defect maps may come from the EEPROM.

### 3.3 Lens Shading Correction (LSC)
Lenses transmit less light at the corners → vignetting + color shading. LSC applies a
per-position gain map (often from EEPROM calibration) to flatten brightness and color
across the frame.

### 3.4 White Balance (AWB application)
Multiply R, G, B by gains so neutral objects are neutral (see Module 1 §7). The gains
come from the **AWB algorithm** using ISP statistics.

### 3.5 Demosaic (the big one)
Each pixel has only one color (R, G, or B). Demosaic interpolates the missing two
colors from neighbors, producing full RGB per pixel.

```
 Bayer (1 color/pixel)            After demosaic (3 colors/pixel)
  R  G  R  G                       RGB RGB RGB RGB
  G  B  G  B          ──►          RGB RGB RGB RGB
  R  G  R  G                       RGB RGB RGB RGB
```

Poor demosaic causes **zippering** (edge artifacts) and **false color** at fine
detail. This is the most compute-intensive ISP stage.

### 3.6 Color Correction Matrix (CCM)
Sensor color filters don't match human cone response. A calibrated 3×3 matrix maps
sensor RGB → standard (sRGB) RGB:

```
 [R']   [ a11 a12 a13 ] [R]
 [G'] = [ a21 a22 a23 ] [G]
 [B']   [ a31 a32 a33 ] [B]
```

Wrong CCM = wrong hues (skin tones off, etc.). It's calibrated per module/illuminant.

### 3.7 Gamma and tone mapping
- **Gamma** applies a non-linear curve (~2.2) to match display response and human
  perception (we see more detail in shadows). Encodes linear sensor data to gamma
  space.
- **Tone mapping** (esp. for HDR) compresses high-dynamic-range linear data into the
  display range while preserving local contrast (local tone mapping, LTM).

### 3.8 Noise reduction
- **2D spatial NR** — smooths within a frame (risks blurring detail).
- **Temporal NR (TNR / 3D)** — averages across frames using motion compensation;
  needs a reference frame buffer in DDR (extra DMA bandwidth — Module 16).
- **Chroma vs luma NR** tuned separately.

### 3.9 Sharpening
Edge enhancement to recover perceived detail lost to demosaic/NR. Over-sharpening →
halos and amplified noise.

### 3.10 Color space conversion + scaler + crop
Convert RGB→YUV (usually **NV12**: Y plane + interleaved UV, 4:2:0) for efficient
encode/display, then **scale/crop** to the requested output resolution. The ISP often
has multiple output paths (full-res for video + downscaled for preview).

---

## 4. Color spaces you must know

```
RGB        : per-pixel red/green/blue. Used internally & for display.
YUV/YCbCr  : Y = luma (brightness), U/Cb & V/Cr = chroma (color difference).
             Eyes are less sensitive to chroma → subsample it.

Subsampling notation (J:a:b):
  4:4:4  no chroma subsampling (full)
  4:2:2  chroma horizontally halved  (YUYV/UYVY) — common camera bus format
  4:2:0  chroma halved H & V          (NV12/NV21/I420) — most common storage/encode
```

**NV12** (4:2:0, semi-planar: Y plane then interleaved UV) is the workhorse camera
output format because video encoders and GPUs consume it directly with zero copy.

```
NV12 layout in DDR:
 ┌─────────────────────┐
 │ Y plane (w×h bytes) │
 ├─────────────────────┤
 │ UV plane (w×h/2)    │  interleaved U,V,U,V...
 └─────────────────────┘
```

---

## 5. The 3A algorithms (AE / AWB / AF)

The "3A" control loops run on **statistics** the ISP computes per frame and DMA's to
memory; the algorithms (in HAL/userspace, sometimes firmware) read stats and push new
settings.

```
            ┌──────────── ISP STATS (per frame) ────────────┐
            │  AE: luma histogram, region averages          │
            │  AWB: per-region R/G/B sums (gray-world etc.)  │
            │  AF: high-frequency contrast / PDAF phase      │
            └───────────────────┬───────────────────────────┘
                                │
                    ┌───────────▼───────────┐
                    │  3A algorithms (HAL)   │
                    └───────────┬───────────┘
        ┌───────────────────────┼────────────────────────┐
        ▼                       ▼                         ▼
  AE → sensor exposure/   AWB → ISP WB gains      AF → VCM focus code
       gain (V4L2 ctrls)        (ISP regs)             (lens subdev ctrl)
```

- **AE (Auto Exposure):** keeps image brightness on target by adjusting sensor
  exposure + gain (and sometimes frame rate). Uses luma histogram / region metering.
- **AWB (Auto White Balance):** estimates the illuminant and sets R/G/B gains so
  neutrals are neutral. Gray-world, white-patch, illuminant-estimation methods.
- **AF (Auto Focus):** maximizes sharpness. **Contrast AF** sweeps the VCM looking for
  peak high-frequency energy; **PDAF** (phase detect) uses dedicated phase pixels for
  single-shot focus direction/distance.

As a kernel engineer your job is the **plumbing**: configure stats engines, route
their DMA, deliver stats buffers to userspace (often via a V4L2 **metadata** video
node), and apply results atomically using the **V4L2 Request API** so settings land on
the correct frame (Module 6).

---

## 6. Where the ISP lives in Linux

Two architectural patterns:

```
(A) Kernel-visible ISP (mainline, per-block control):
    sensor subdev → CSI subdev → ISP subdev(s) → video node
    Each ISP block configured via V4L2 controls / sub-device.
    Stats & params via V4L2 metadata nodes (e.g. rkisp1, i.MX).

(B) Firmware/black-box ISP (Qualcomm CamX, NVIDIA Argus):
    Kernel just moves RAW + config blobs to an ISP firmware/DSP;
    proprietary 3A runs there. Kernel sees opaque param/stats buffers.
```

- **rkisp1** (`drivers/media/platform/rockchip/rkisp1`) is the best *open* example of
  pattern (A): it exposes `params` and `stats` metadata video nodes with documented
  UAPI structs (`include/uapi/linux/rkisp1-config.h`).
- **Qualcomm** uses pattern (B): the IFE does hardware processing but tuning + 3A live
  in CamX/CHI userspace; the kernel CAMSS driver just feeds it (Module 11).
- **libcamera** (userspace) increasingly hosts the open 3A algorithms (IPA modules)
  for pattern (A) ISPs.

---

## 7. Putting it together: a frame's journey through the ISP

```
 t0  Sensor emits RAW10 frame N over CSI
 t1  CSID validates packets, routes to IFE/ISP input
 t2  ISP: BLC → LSC → demosaic → CCM → gamma → NR → CSC → scale
 t3  ISP write-master DMAs NV12 frame N to DDR buffer (vb2)
 t3  ISP stats engine DMAs AE/AWB/AF stats for frame N to stats buffer
 t4  vb2 marks frame N done → userspace dequeues; stats node delivers stats
 t5  3A computes settings for frame N+2 (pipeline latency)
 t6  Settings applied via Request API, tagged to land on frame N+2
```

Note the **2-frame latency**: stats from frame N affect frame N+2, because frame N+1
is already mid-flight. This is why the **Request API** (atomic per-frame parameter
sets) exists — Module 6.

---

## 8. Debugging ISP-layer problems by symptom

```
Symptom                         Likely ISP stage at fault
────────────────────────────────────────────────────────────────
Washed-out / milky blacks       Black Level Correction (pedestal)
Dark corners / color in corners Lens Shading Correction
False colors at fine detail     Demosaic (or wrong Bayer phase upstream)
Wrong hues / bad skin tone      Color Correction Matrix / AWB
Too dark or too bright overall  AE loop / exposure not applied
Color cast (whole image tinted) AWB gains wrong / illuminant misdetected
Blurry but "clean"              Over-aggressive noise reduction
Halos around edges, noisy edges Over-sharpening
Blocky color / banding          Gamma/tone curve or bit-depth loss
Ghosting on moving objects      Temporal NR with bad motion compensation
```

Tools: dump ISP params/stats from the metadata nodes, capture the **RAW before ISP**
(many pipelines allow a RAW bypass path) to isolate sensor vs ISP, and use
`v4l2-ctl`/`media-ctl` to inspect/force block settings (Module 15).

---

## 9. Interview Q&A

**Q1. The image has correct exposure but a green tint. Sensor or ISP?**
ISP — specifically AWB. A uniform color cast means white balance gains are wrong (or
the illuminant was misestimated). Exposure being correct rules out AE/sensor.

**Q2. Why is demosaic the most expensive ISP stage and what artifacts does bad
demosaic cause?**
It interpolates two missing colors at every pixel using neighborhood math at full
resolution — huge per-pixel compute. Bad demosaic causes zippering on edges and false
color/moiré on fine detail.

**Q3. What are ISP statistics and what is your role as a kernel engineer regarding
them?**
Per-frame summaries (luma histograms, region color sums, AF contrast/phase) the ISP
DMAs to memory for the 3A algorithms. The kernel configures the stats engines, routes
their DMA, and delivers stats buffers to userspace (often a V4L2 metadata node); it
does not run the 3A math itself in firmware-ISP designs.

**Q4. Why is there a 2–3 frame latency in the 3A loop?**
Stats for frame N are only available after N is processed, by which time N+1 is
already exposing/in-flight, so new settings can only take effect on N+2. This pipeline
delay motivates the Request API for per-frame atomic settings.

**Q5. Why output NV12 instead of RGB?**
NV12 is YUV 4:2:0 semi-planar: it halves chroma data (smaller, less bandwidth) and is
the native input for hardware video encoders and GPUs, enabling zero-copy. RGB wastes
bandwidth and isn't directly encoder-friendly.

**Q6. Difference between contrast AF and PDAF?**
Contrast AF sweeps the lens (VCM) and maximizes high-frequency image energy — slow,
"hunting." PDAF uses dedicated phase-detection pixels to compute focus direction and
magnitude in one shot — fast, used in phones/ADAS.

**Q7. In a firmware ISP (Qualcomm/NVIDIA) what does the kernel actually do for image
processing?**
It moves data and config: feeds RAW frames and opaque tuning/parameter blobs to the
ISP hardware/firmware, manages buffers and DMA/SMMU, handles interrupts and
done-events, and returns stats buffers — but the actual 3A and tuning math runs in the
proprietary firmware/userspace (CamX, Argus), not the kernel.

**Q8. Where would lens shading calibration data come from and how is it applied?**
From the module's EEPROM/OTP calibration. Userspace/HAL reads it and programs the
ISP's LSC block (gain maps) so vignetting and corner color shading are flattened.

---

### Key takeaways
- RAW Bayer is not viewable; the ISP pipeline (BLC→LSC→demosaic→CCM→gamma→NR→CSC)
  turns it into NV12/YUV.
- Pipeline order is fixed by image math, so artifacts localize to specific stages.
- 3A (AE/AWB/AF) is a control loop fed by ISP statistics DMA; the kernel plumbs stats
  and applies results, with ~2-frame latency handled by the Request API.
- Mainline ISPs (rkisp1) expose per-block control + metadata nodes; Qualcomm/NVIDIA
  hide 3A in firmware and the kernel just feeds the ISP.
