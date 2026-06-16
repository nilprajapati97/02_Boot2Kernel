# Module 1 — Introduction to Digital Cameras

> **Scope:** Foundations of how a real camera forms an image, written for a Linux
> kernel/BSP engineer. Before you can write a sensor driver or debug a MIPI CSI-2
> link, you must understand *what the hardware is physically doing* — photons,
> exposure, gain, Bayer color, and the analog/digital boundary. This module is the
> conceptual bedrock for Modules 2–18.

---

## 0. Why a kernel engineer must understand optics first

You will spend your career writing code like this:

```c
v4l2_ctrl_new_std(&sensor->ctrls, &imx_ctrl_ops,
                  V4L2_CID_EXPOSURE, 1, 65535, 1, 1000);
v4l2_ctrl_new_std(&sensor->ctrls, &imx_ctrl_ops,
                  V4L2_CID_ANALOGUE_GAIN, 0, 978, 1, 0);
```

If you do not understand **what exposure and analogue gain physically mean**, you
cannot:

- Choose sane min/max/default control ranges from a sensor datasheet.
- Debug "image too dark" or "image too noisy" bug reports.
- Explain to the ISP/3A team why frames are over/under exposed.
- Reason about rolling-shutter artifacts that look like a CSI timing bug but aren't.

So Module 1 maps every photographic concept onto the **register / driver concept**
you will actually touch.

```
Photographic concept     →   Sensor register / V4L2 control you will program
────────────────────────────────────────────────────────────────────────────
Exposure time            →   coarse/fine integration time regs  / V4L2_CID_EXPOSURE
Sensitivity (ISO)        →   analog + digital gain regs          / V4L2_CID_ANALOGUE_GAIN
Aperture                 →   (fixed on most embedded modules; VCM only moves focus)
Focus                    →   VCM DAC code                        / V4L2_CID_FOCUS_ABSOLUTE
White balance            →   per-channel digital gain (ISP/AWB)  / V4L2_CID_*_WHITE_BALANCE
Frame rate               →   frame length lines (VTS) + pixclk   / VIDIOC_S_PARM
Resolution / crop / bin  →   windowing + binning regs            / VIDIOC_S_FMT / S_SELECTION
```

---

## 1. A one-paragraph history (why it matters)

Cameras evolved from **chemical film** (silver-halide grains darken with light) to
**CCD** (charge shuffled bucket-brigade to one ADC) to **CMOS** (each pixel has its
own readout, column-parallel ADCs). The kernel only ever talks to **CMOS** sensors
today, because CMOS is low-power, supports on-die ADC, region-of-interest readout,
and integrates control logic addressable over I2C. Every concept below (exposure,
gain, rolling shutter) is a direct consequence of the **CMOS pixel + row-by-row
readout** architecture you will drive.

---

## 2. Analog vs digital camera — where the kernel boundary is

```
   PHYSICAL WORLD (analog)                 DIGITAL DOMAIN (your driver)
 ┌──────────────────────────┐   ADC      ┌──────────────────────────────┐
 │ Photons → photodiode →   │ ────────►  │ RAW Bayer pixels → MIPI CSI-2 │
 │ charge → voltage         │            │ → ISP → DMA → DDR             │
 └──────────────────────────┘            └──────────────────────────────┘
        ▲ analog gain here                     ▲ digital gain / AWB here
```

The **ADC inside the sensor** is the analog→digital boundary. Everything left of it
is physics you influence only indirectly (exposure time, analog gain). Everything
right of it is data your kernel pipeline moves and processes. Knowing which side a
problem lives on is half of camera debugging.

---

## 3. Image formation — the optical chain

```
 Scene → Lens → Aperture → IR-cut filter → Color filter array → Pixel array
        (focus) (light qty)  (block IR)      (Bayer RGGB)        (photodiodes)
```

1. **Lens** focuses incoming light to a sharp plane (the sensor surface).
2. **Aperture** limits how much light passes (f-number). On embedded modules it is
   almost always **fixed**; only DSLRs and a few phones have variable apertures.
3. **IR-cut filter** removes infrared so colors look correct to humans. (Removed on
   night-vision/security cameras — that's why they go monochrome + IR-LED at night.)
4. **Color filter array (CFA)** — a Bayer mosaic placing R/G/B filters over pixels.
5. **Pixel array** — millions of photodiodes converting photons to charge.

### 3.1 The exposure triangle (the one diagram to memorize)

```
                Brightness of final image
                          ▲
        ┌─────────────────┼─────────────────┐
        │                 │                 │
   Exposure time      Aperture           ISO / Gain
   (shutter)          (f-number)         (amplification)
   longer = brighter  wider = brighter   higher = brighter
   + motion blur      + shallow DoF      + more noise
```

For embedded sensors, aperture is fixed, so the **AE (auto-exposure) algorithm only
has two knobs: exposure time and gain.** That is exactly what your sensor driver
exposes via `V4L2_CID_EXPOSURE` and `V4L2_CID_ANALOGUE_GAIN`.

---

## 4. Lens, aperture, focus

### 4.1 Focal length and field of view
Focal length (mm) sets the field of view. Short focal length = wide angle; long =
telephoto. On a fixed module this is a hardware property you report, not control.

### 4.2 Aperture and f-number
`f-number = focal_length / aperture_diameter`. Smaller f-number (f/1.8) = larger
opening = more light + shallower depth of field. Larger f-number (f/8) = less light
+ deeper DoF.

### 4.3 Focus and the VCM
Embedded modules focus by physically moving the lens with a **Voice Coil Motor
(VCM)**, driven by a tiny I2C DAC (e.g. DW9714, AK7375).

```
 AF driver writes DAC code → VCM current → lens moves → focus plane shifts
```

In Linux this is a separate **lens sub-device** (`drivers/media/i2c/dw9714.c`)
exposing:

```c
V4L2_CID_FOCUS_ABSOLUTE   /* DAC code: 0 = infinity ... max = macro */
```

The AF algorithm (in the ISP/userspace) sweeps this code and measures contrast or
phase-detect statistics to find best focus. **You, the driver author, only provide
the knob; the 3A algorithm turns it.**

---

## 5. Shutter, exposure, and the rolling-shutter trap

### 5.1 What "exposure" is at the silicon level
Exposure = how long each pixel integrates charge before readout. In a CMOS sensor
this is set in **units of lines**, not microseconds:

```
exposure_time = coarse_integration_lines × line_time
line_time     = line_length_pixels (HTS) / pixel_clock
```

So when you write `V4L2_CID_EXPOSURE = 1000`, you are writing **1000 lines** of
integration into the sensor's coarse-integration register. The driver (or ISP)
converts the user's microseconds to lines using HTS/pixclk.

### 5.2 Rolling shutter vs global shutter — critical for debugging

```
ROLLING SHUTTER (most CMOS):              GLOBAL SHUTTER (industrial/automotive):
 Row 0 exposed at t0 ─────►               ALL rows exposed simultaneously
 Row 1 exposed at t0+Δ ────►              then read out row by row
 Row 2 exposed at t0+2Δ ───►
   ...                                    No skew, no jello, but needs storage
 Each row starts later → skew              node per pixel → larger pixel, costlier
```

**Rolling shutter artifacts** (skewed propellers, "jello" wobble, partial-frame
banding under LED/flicker light) are *physics*, not a CSI bug. A frequent junior
mistake is to chase a "tearing" artifact in the DMA path when the sensor is simply
rolling. Knowing this saves days.

Global shutter sensors (e.g. Sony Pregius IMX296) expose all rows at once — required
for machine vision, drones, and ADAS where motion skew is unacceptable.

---

## 6. ISO, gain, and noise

### 6.1 ISO = sensitivity = gain
"ISO" is the photography word for **gain applied to the sensor signal**. Higher ISO
brightens dark scenes but amplifies noise because gain amplifies signal *and* noise
equally; it cannot improve signal-to-noise ratio.

```
Analog gain   : amplifies the analog pixel voltage BEFORE the ADC (best SNR)
Digital gain  : multiplies digital code AFTER the ADC (just scales noise too)
```

Always prefer **analog gain** first, then digital gain — which is why
`V4L2_CID_ANALOGUE_GAIN` and `V4L2_CID_DIGITAL_GAIN` are separate controls and the
AE algorithm fills analog before digital.

### 6.2 Noise sources you must be able to name
- **Shot noise** — fundamental Poisson noise of photon arrival (√N). Unavoidable.
- **Read noise** — added by readout/ADC electronics.
- **Dark current / thermal noise** — charge accumulating without light (worse hot).
- **Fixed-pattern noise (FPN)** — per-pixel offset/gain variation; corrected by ISP.
- **Quantization noise** — from finite ADC bit depth (RAW8 vs RAW12 matters).

---

## 7. Color temperature and white balance

Light has a color temperature (Kelvin): candle ~1800K (orange), daylight ~5600K,
overcast ~7000K (blue). The sensor records whatever color the light actually is, so
a white wall looks orange under tungsten. **White balance (AWB)** rescales the R/G/B
channels so neutral objects appear neutral.

```
AWB output = per-channel gains:  R_gain, G_gain, B_gain
applied either in the sensor digital gain regs or in the ISP white-balance block.
```

This is an **ISP/3A** function (Module 4), but the sensor driver may expose
`V4L2_CID_RED/BLUE_BALANCE` for sensors that do WB on-chip.

---

## 8. Depth of field, frame rate, resolution

- **Depth of field (DoF):** range of distance in focus. Wide aperture / long focal
  length / close subject → shallow DoF (blurry background).
- **Frame rate (fps):** frames per second. Set by total frame time:
  `frame_time = VTS (frame_length_lines) × line_time`. To raise fps you reduce VTS
  (fewer lines) or raise pixel clock. Exposure must fit inside the frame:
  `exposure ≤ VTS`. This coupling is a classic interview question.
- **Resolution:** pixel count (e.g. 1920×1080). Higher resolution = more data per
  frame = more MIPI bandwidth + more DDR + more DMA — the kernel cost you manage.

---

## 9. HDR, dynamic range, tone mapping

- **Dynamic range:** ratio between brightest and darkest signal a sensor captures
  in one frame, expressed in dB or stops. A single 12-bit exposure can't hold both a
  bright sky and a dark shadow.
- **HDR techniques:**
  - **Multi-exposure / stagger HDR:** capture long + short exposures, merge. The
    sensor emits two/three exposures per frame on different MIPI **virtual channels**
    or interleaved lines — directly affecting your CSI/CSID driver (Module 8/11).
  - **DOL (Digital Overlap) HDR:** Sony's line-interleaved scheme.
  - **PWL/companding:** sensor compresses 16–20 bit linear data into RAW12 using a
    piecewise-linear curve; ISP decompands it.
- **Tone mapping:** compresses high dynamic range down to an 8-bit display range
  while preserving local contrast — an ISP function (Module 4).

HDR matters to the **kernel** because it multiplies data rate and adds virtual
channels / metadata the CSID and ISP must route. "Why does the IFE see two frames
per sensor frame?" → stagger HDR.

---

## 10. Motion blur

Motion blur = subject moves during the exposure window. Reduced by shorter exposure
— but shorter exposure needs more gain (more noise) or more light. This is the AE
tradeoff again. For automotive/ADAS the rule is "freeze motion" → short exposure +
HDR to keep dynamic range.

---

## 11. End-to-end mental model (carry this through all 18 modules)

```
 Photons
   │  (Module 1: optics, exposure, gain)
   ▼
 Lens + CFA + Photodiode  ── analog ──┐
   │                                  │ (Module 3: CMOS sensor)
   ▼                                  ▼
 ADC ─ RAW Bayer ─► MIPI CSI-2 ─► CSI RX / CSID
   │  (Module 8: CSI-2)        (Module 11/12: SoC RX)
   ▼
 ISP (demosaic, 3A, NR, tonemap)   (Module 4 concept, Module 10/11 driver)
   │
   ▼
 DMA engine ─► DDR (Module 13 vb2, Module 14 IOMMU/DMA)
   │
   ▼
 V4L2 / Media Controller userspace (Module 5/6)
   │
   ▼
 Android Camera HAL3 / Argus (Module 12/17)
```

Every later module is "zoom in on one box of this diagram and write/debug the
driver for it."

---

## 12. Interview Q&A

**Q1. A bug report says "image is dark and noisy indoors." Walk the chain.**
Indoors = low light. AE raised exposure to its ceiling (limited by frame time / fps)
and then raised gain → amplifies noise → dark+noisy. Fix options: lower fps to allow
longer exposure, add light, accept noise, or improve NR. Nothing is broken; it's the
exposure triangle. Verify by reading the sensor's exposure/gain registers.

**Q2. Why is exposure expressed in lines, not microseconds, in a sensor driver?**
Because integration is controlled relative to the row readout clock. `exposure_us =
coarse_lines × line_length / pixel_clock`. The driver converts user microseconds to
the sensor's native line units.

**Q3. A spinning fan looks bent/skewed. Is this a CSI DMA bug?**
No — rolling shutter. Each row is exposed at a slightly later time, so fast motion
appears skewed ("jello"). Use a global-shutter sensor if unacceptable. It is physics,
not a transport bug.

**Q4. Difference between analog and digital gain, and which does AE use first?**
Analog gain amplifies the pixel voltage before the ADC and preserves SNR best;
digital gain multiplies post-ADC codes and amplifies noise too. AE fills analog gain
first, then digital only when analog is exhausted.

**Q5. Why does enabling HDR sometimes make the IFE/CSID report two frames per
sensor frame?**
Stagger/DOL HDR emits multiple exposures per frame, often on separate MIPI virtual
channels or interleaved lines. The CSID demuxes them into separate streams the ISP
merges. It is expected, not a frame-doubling bug.

**Q6. How are frame rate and maximum exposure related?**
`frame_time = VTS × line_time` and `exposure ≤ VTS`. Higher fps ⇒ smaller VTS ⇒
smaller maximum exposure. To shoot 120fps you cap exposure to ~1/120s minus blanking,
which usually forces more gain (noise) in low light.

**Q7. What does the IR-cut filter do and when is it removed?**
It blocks infrared so colors look natural to humans. Security/night-vision cameras
remove it (mechanically swing it out) at night and use IR illumination, producing a
monochrome IR image.

---

### Key takeaways
- The ADC inside the sensor is the analog/digital boundary; know which side a bug is on.
- AE has only two embedded knobs: exposure (in lines) and gain (analog then digital).
- Rolling shutter artifacts are physics, not transport bugs.
- HDR multiplies data rate and adds virtual channels — a kernel concern, not just optics.
- Every later module zooms into one box of the photons→DDR→userspace chain.
