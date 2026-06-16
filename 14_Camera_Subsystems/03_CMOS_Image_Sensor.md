# Module 3 — CMOS Image Sensor

> **Scope:** Deep dive into how a CMOS image sensor physically works and how every
> internal block maps to a register you program from a Linux sensor driver. Covers
> pixel/photodiode, Bayer color, ADC, exposure/gain readout, rolling vs global
> shutter, PLL/timing, binning/crop/scale, and RAW output formats. This is the
> datasheet-to-driver layer beneath Module 9.

---

## 1. Why this module matters

When you read a Sony IMX or OmniVision OV datasheet to write a driver, you'll meet
registers named `COARSE_INTEG_TIME`, `ANA_GAIN_GLOBAL`, `HTS`, `VTS`, `X_ADDR_START`,
`PLL_MULT`. None of them make sense unless you understand the pixel array, readout,
and timing model below. This module gives you that model so the register table stops
being magic.

```
Datasheet block        →  Register group        →  V4L2 control / driver field
──────────────────────────────────────────────────────────────────────────────
Pixel integration      →  COARSE/FINE_INTEG     →  V4L2_CID_EXPOSURE
Analog amplifier       →  ANA_GAIN_GLOBAL       →  V4L2_CID_ANALOGUE_GAIN
Digital multiplier     →  DIG_GAIN_*            →  V4L2_CID_DIGITAL_GAIN
Line timing (HTS)      →  LINE_LENGTH_PCK       →  used to compute exposure & fps
Frame timing (VTS)     →  FRM_LENGTH_LINES      →  VIDIOC_S_PARM (fps)
Windowing              →  X/Y_ADDR_START/END    →  V4L2 selection / crop
Binning                →  BINNING_MODE          →  affects advertised frame size
PLL                    →  PLL_MULT/PREDIV       →  V4L2_CID_LINK_FREQ
Output format          →  CSI_DT / RAW depth    →  media bus code (MEDIA_BUS_FMT_*)
```

---

## 2. The pixel and photodiode

```
        Incoming photons
              │
        ┌─────▼─────┐  microlens (focuses light onto photodiode)
        │  µlens    │
        ├───────────┤  color filter (R, G, or B)
        │  filter   │
        ├───────────┤
        │ photodiode│  photons → electron-hole pairs → charge well
        ├───────────┤
        │ transfer  │  TX gate moves charge to floating diffusion
        │  +  ADC   │  converted to voltage, amplified, digitized
        └───────────┘
```

A pixel:
1. Collects photons in the **photodiode** during the integration (exposure) window.
2. The accumulated charge is proportional to light × time.
3. A **transfer gate** dumps charge to a **floating diffusion** node → voltage.
4. **Correlated double sampling (CDS)** subtracts reset noise.
5. The **column ADC** digitizes the voltage to a RAW code.

**Full-well capacity** (max electrons) and **read noise** set the dynamic range. Pixel
size (e.g. 1.0 µm vs 2.0 µm) trades resolution against light sensitivity — bigger
pixels = better low light, smaller = more megapixels.

---

## 3. The 4T CMOS pixel (what "active pixel sensor" means)

Modern CMOS uses a **4-transistor (4T)** pixel: transfer gate (TX), reset (RST),
source-follower (SF) amplifier, and row-select (SEL). Each pixel amplifies its own
signal (hence "active pixel sensor"), enabling column-parallel readout — the basis of
rolling shutter and high frame rates. Compare to **CCD**, where charge is physically
shuffled to a single output amplifier (slower, more power, no per-pixel addressing).

---

## 4. Bayer pattern and the color filter array

A monochrome photodiode can't see color, so a **Color Filter Array (CFA)** places one
color filter per pixel. The dominant CFA is the **Bayer pattern** — 50% green (eye is
most sensitive to green), 25% red, 25% blue:

```
 RGGB           BGGR           GRBG           GBRG
 R G R G        B G B G        G R G R        G B G B
 G B G B        G R G R        B G B G        R G R G
 R G R G        B G B G        G R G R        G B G B
 G B G B        G R G R        B G B G        R G R G
```

The **starting phase** (which color is top-left) depends on the sensor and on any
crop offset. Getting it wrong gives swapped/false colors after demosaic — a very
common bug. In Linux this phase is encoded in the **media bus code**:

```c
MEDIA_BUS_FMT_SRGGB10_1X10   /* RGGB, 10-bit */
MEDIA_BUS_FMT_SBGGR10_1X10   /* BGGR, 10-bit */
MEDIA_BUS_FMT_SGRBG10_1X10   /* GRBG, 10-bit */
MEDIA_BUS_FMT_SGBRG10_1X10   /* GBRG, 10-bit */
```

If you crop by an odd number of pixels/lines, the Bayer phase flips — your driver
must adjust the reported bus code accordingly.

Variants you may meet: **RGBW** (adds white/clear pixels for sensitivity),
**Quad-Bayer / Tetracell** (2×2 same-color groups, re-mosaiced for high-res or
binned for low light), and **RCCB/RCCC** (automotive, red+clear for headlight/sign
detection).

---

## 5. ADC, exposure, gain, and readout

### 5.1 Exposure (integration time)
Set in **lines**:
```
exposure_time = COARSE_INTEG_TIME × line_time
line_time     = LINE_LENGTH_PCK (HTS) / pixel_clock
```
The driver/ISP converts user microseconds ↔ lines. Constraint: `exposure ≤ VTS −
margin` (can't integrate longer than the frame).

### 5.2 Gain
- **Analog gain** (`ANA_GAIN_GLOBAL`): amplifies before the ADC, best SNR. Often a
  non-linear register code: `gain = 1 / (1 − code/512)` style mapping (read the
  datasheet — it differs per sensor).
- **Digital gain** (`DIG_GAIN`): post-ADC multiply, amplifies noise too.

Your driver implements `s_ctrl` to translate the V4L2 gain value into the sensor's
specific register encoding.

### 5.3 Readout
After integration, rows are read out one at a time through column ADCs. Readout time
per row = line_time. Total frame readout spans VTS lines. This row-sequential readout
is exactly what produces **rolling shutter**.

---

## 6. Rolling shutter vs global shutter (silicon view)

```
ROLLING (4T standard):                 GLOBAL (5T / charge-storage):
 reset→integrate→read, row by row       all pixels reset+integrate together,
 each row delayed by line_time           charge stored per-pixel, then read out
 cheap, small pixel                       extra storage node → bigger/costlier pixel
 motion skew ("jello")                    no skew — machine vision / ADAS
```

Global-shutter sensors (Sony Pregius, OnSemi AR0234) add a per-pixel storage node.
Drivers expose this via the read-only control `V4L2_CID_PIXEL_RATE` /
`V4L2_CID_VBLANK` knobs, and the media bus codes are identical; the difference is
sensor capability, not API.

---

## 7. Sensor timing: HTS, VTS, line_time, frame_time

```
        ◄──────────── LINE_LENGTH_PCK (HTS) ───────────►
        ┌───────────────────────────┬──────────────────┐
 line 0 │  active pixels (e.g.1920)  │ horizontal blank │
        ├───────────────────────────┼──────────────────┤
 line 1 │  ...                       │                  │
        │  ...                       │                  │   ▲
        ├───────────────────────────┴──────────────────┤   │ FRM_LENGTH_LINES
 active │  active lines (e.g. 1080)                     │   │ (VTS)
 region │                                               │   │
        ├───────────────────────────────────────────────┤   │
 vblank │  vertical blanking lines                      │   ▼
        └───────────────────────────────────────────────┘

 line_time  = HTS / pixel_clock
 frame_time = VTS × line_time
 fps        = 1 / frame_time = pixel_clock / (HTS × VTS)
```

**To change fps** you change **VTS** (vertical blanking). To change exposure you
change `COARSE_INTEG_TIME` within VTS. The relationship `fps = pixclk/(HTS×VTS)` is a
guaranteed interview question. In V4L2 these are exposed as `V4L2_CID_HBLANK` and
`V4L2_CID_VBLANK` controls plus `V4L2_CID_PIXEL_RATE`.

---

## 8. PLL and clock tree

```
 XCLK (24 MHz) → ÷PRE_DIV → ×PLL_MULT → ÷POST_DIV → VT pixel clock (pixel readout)
                                          → MIPI bit clock (link)
```

The sensor register table programs the PLL dividers/multipliers. The resulting
**link frequency** must be advertised to the SoC via:

```c
static const s64 link_freq_menu[] = { 456000000, };   /* Hz */
v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_LINK_FREQ,
                       ARRAY_SIZE(link_freq_menu) - 1, 0, link_freq_menu);
```

The CSIPHY driver reads `V4L2_CID_LINK_FREQ` to configure D-PHY settle timing. Mismatch
= CRC errors. (Detailed in Modules 8 and 9.)

---

## 9. Binning, cropping, scaling, skipping

These reduce resolution/bandwidth and are selected per **sensor mode**:

```
CROP (windowing): read only a sub-rectangle (X/Y_ADDR_START/END)
                  → smaller FOV, full pixel detail (ROI / digital zoom source)

SKIPPING:         drop every other pixel/line → smaller image, aliasing, fast

BINNING (2x2):    sum/average 2×2 same-color pixels → 1/4 resolution,
                  better SNR & low-light, lower bandwidth, lower fps cost

SCALING:          on-sensor downscale (some sensors) → arbitrary output size
```

A **sensor mode table** in the driver lists each supported (width, height, bus_code,
fps, binning) combo. The driver advertises these via `enum_frame_size` /
`enum_mbus_code`. Binning changes the effective pixel rate and sometimes the Bayer
phase — handle both.

```c
struct imx_mode {
    u32 width;
    u32 height;
    u32 hts;            /* line_length_pck */
    u32 vts_def;        /* frame_length_lines */
    const struct reg_value *reg_list;   /* register table for this mode */
};
```

---

## 10. Output formats: RAW8/10/12/14 and CSI data types

The sensor outputs **RAW Bayer** at a chosen bit depth. Each maps to a **MIPI CSI-2
data type (DT)** and a **media bus code**:

```
RAW depth   CSI-2 Data Type     Media bus code example          Notes
──────────────────────────────────────────────────────────────────────────
RAW8        0x2A                MEDIA_BUS_FMT_SRGGB8_1X8         lowest BW, most noise visible
RAW10       0x2B                MEDIA_BUS_FMT_SRGGB10_1X10       most common
RAW12       0x2C                MEDIA_BUS_FMT_SRGGB12_1X12       better DR
RAW14       0x2D                MEDIA_BUS_FMT_SRGGB14_1X14       high-end / HDR
```

On the bus the data is packed (e.g. RAW10 = 4 pixels in 5 bytes — "MIPI packed").
After the CSID/ISP it may be unpacked to 16-bit per pixel in DDR. The packed vs
unpacked distinction matters for **buffer size and stride** calculations (Modules 6,
13). Getting bytesperline wrong → sheared image.

---

## 11. Frame synchronization and multi-camera

For stereo/surround systems multiple sensors must be **frame-synchronized**. Methods:
- **Master/slave (hardware sync):** one sensor's FSIN/XVS output drives the others'
  sync input so all expose simultaneously.
- **External trigger:** a SoC timer/GPIO pulses all sensors' FSIN.

The sensor exposes sync via XVS/XHS pins; the driver may toggle a control or it's a
board-level wiring concern. Critical for stereo depth and surround-view stitching.

---

## 12. Sensor registers and the driver register table

A sensor mode is applied by writing a long list of register/value pairs:

```c
static const struct reg_value imx219_mode_1920x1080[] = {
    {0x0164, 0x02}, {0x0165, 0xa8},   /* X_ADD_STA */
    {0x0166, 0x0a}, {0x0167, 0x27},   /* X_ADD_END */
    {0x0168, 0x02}, {0x0169, 0xb4},   /* Y_ADD_STA */
    ...
    {0x0160, 0x04}, {0x0161, 0x59},   /* FRM_LENGTH (VTS) */
    {0x0162, 0x0d}, {0x0163, 0x78},   /* LINE_LENGTH (HTS) */
};
```

These come straight from the sensor vendor's "setting file." Your driver streams them
over I2C/CCI in `start_streaming`, then writes exposure/gain/VTS on top from the
control handlers. (Full driver in Module 9.)

---

## 13. Real Sony IMX example (mental anchor)

Sony IMX sensors (IMX219, IMX477, IMX290, IMX678) are the reference everyone uses:
- 16-bit register addresses, 8-bit data.
- Standard SMIA++/CCS-like register map (`0x0160 FRM_LENGTH`, `0x0162 LINE_LENGTH`,
  `0x0202 COARSE_INTEG`, `0x0157 ANA_GAIN`).
- Support DOL-HDR (line-interleaved multi-exposure) on the higher-end parts.
- Mainline drivers exist (`drivers/media/i2c/imx219.c`, `imx290.c`) — read these as
  canonical examples.

---

## 14. Debugging the sensor layer

```bash
# Dump/peek sensor registers via the i2c sub-device debugfs or v4l2 ctrls
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls          # exposure/gain ranges
v4l2-ctl -d /dev/v4l-subdev0 --get-ctrl exposure

# Confirm advertised modes / bus codes
media-ctl -p                                        # shows subdev pad formats

# Scope checks (hardware):
#   XCLK present & correct freq
#   MIPI lanes toggling at expected bit clock
#   I2C ACK on chip-id read
```

Common sensor-layer bugs:
- **Wrong chip-ID at probe** → wrong I2C addr, no power, or no XCLK.
- **All-green or false-color image** → wrong Bayer phase / bus code, or crop offset
  flipped the phase.
- **Image too bright/dark and won't respond to exposure** → control not actually
  written (check `s_ctrl`), or units (lines vs µs) wrong.
- **Wrong fps** → VTS not programmed / pixel_rate advertised incorrectly.
- **Sheared / diagonal image** → bytesperline/stride wrong vs packed RAW format.

---

## 15. Interview Q&A

**Q1. Derive frame rate from sensor timing registers.**
`fps = pixel_clock / (HTS × VTS)`, where HTS = LINE_LENGTH_PCK and VTS =
FRM_LENGTH_LINES. To lower fps, increase VTS (vertical blanking).

**Q2. Why is green sampled twice as often as red/blue in Bayer?**
The human eye is most sensitive to green (luminance), so 50% green gives better
perceived resolution and SNR for luminance detail; R and B carry chroma at 25% each.

**Q3. You crop the sensor window by one line. What can break?**
The Bayer starting phase flips (e.g. RGGB→GBRG), so the advertised media bus code must
change or demosaic produces false colors. Crop by even offsets to preserve phase, or
update the bus code.

**Q4. Analog vs digital gain — which improves SNR and why?**
Analog gain amplifies the pixel voltage before the ADC, so it lifts signal above the
ADC/read noise floor — improving usable SNR up to a point. Digital gain multiplies
post-ADC codes, scaling signal and noise equally, so it never improves SNR.

**Q5. What limits maximum exposure time at a given fps?**
Exposure must fit within the frame: `exposure ≤ VTS × line_time`. Higher fps ⇒ smaller
VTS ⇒ shorter max exposure. To allow longer exposure you must drop fps (raise VTS).

**Q6. What is MIPI-packed RAW10 and why do you care?**
RAW10 packs 4 pixels into 5 bytes on the CSI bus (no padding). It affects
bytesperline/buffer-size math; if the ISP unpacks to 16-bit in DDR the stride differs.
Miscalculating stride shears the image.

**Q7. How does binning help low light and what does it cost?**
2×2 binning sums four same-color pixels, raising signal (≈4×) faster than noise (≈2×),
improving SNR ~2×, while cutting resolution to 1/4 and reducing required bandwidth.
Cost: lower spatial resolution and possible Bayer-phase considerations.

**Q8. Rolling vs global shutter at the transistor level?**
Rolling = standard 4T pixel read row-by-row, each row time-shifted → motion skew.
Global = 5T (or charge-domain storage) pixel that latches all pixels simultaneously
into a storage node before sequential readout → no skew, larger/costlier pixel.

---

### Key takeaways
- Exposure is in lines; `fps = pixclk/(HTS×VTS)`; exposure ≤ VTS.
- Bayer phase is encoded in the media bus code and flips with odd crop offsets.
- Prefer analog gain (pre-ADC) over digital gain (post-ADC) for SNR.
- A sensor "mode" = a register table + (width,height,bus_code,fps,binning); the driver
  advertises these and layers exposure/gain/VTS on top.
- RAW bit depth ↔ CSI data type ↔ media bus code must stay consistent end to end.
