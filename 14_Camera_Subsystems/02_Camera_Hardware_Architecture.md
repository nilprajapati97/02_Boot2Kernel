# Module 2 — Camera Hardware Architecture

> **Scope:** The complete camera hardware block diagram from a board-bring-up / BSP
> engineer's view. For each block you'll learn what it is, the electrical signals it
> needs (power, clock, reset, I2C/CCI, MIPI), how it appears in the device tree, and
> which Linux driver owns it. This is the "schematic-to-driver" map you use during
> board bring-up.

---

## 1. The complete hardware block diagram

```
                        ┌─────────────┐
                        │    LENS     │  (fixed or VCM-focused)
                        └──────┬──────┘
                               │ light
                        ┌──────▼──────┐
                        │  IR-CUT     │  (optional, blocks IR)
                        └──────┬──────┘
                               │
                        ┌──────▼──────┐      ┌──────────┐
                        │ CMOS SENSOR │◄────►│  EEPROM  │ (calib/OTP, I2C)
                        │  (IMX/OV)   │      └──────────┘
                        │             │◄────►│   VCM    │ (autofocus, I2C)
                        └──┬───┬───┬──┘      └──────────┘
            ┌──────────────┘   │   └──────────────┐
        MIPI CSI-2          I2C/CCI            XCLK (MCLK)
        (D-PHY lanes)     (control bus)       (master clock)
            │                 │                   │
   ┌────────▼──────┐    ┌─────▼─────┐     ┌───────▼──────┐
   │ (opt) Ser/Des │    │ I2C / CCI │     │ Clock source │
   │ GMSL / FPD-III│    │ controller│     │ (PMIC/PLL)   │
   └────────┬──────┘    └───────────┘     └──────────────┘
            │ MIPI
   ┌────────▼────────────────────────────────────────────┐
   │                 SoC CAMERA SUBSYSTEM                 │
   │  CSIPHY → CSID/CSI-RX → IFE/VFE/ISP → DMA (WM)       │
   └────────┬────────────────────────────────────────────┘
            │ AXI / SMMU
   ┌────────▼──────┐
   │  DDR MEMORY   │  (frame buffers, vb2/dma-buf)
   └────────┬──────┘
            │
   ┌────────▼──────┐   ┌─────────────┐   ┌─────────────┐
   │  GPU / Display│   │  Video Enc  │   │  CPU / NN    │
   └───────────────┘   └─────────────┘   └─────────────┘
```

Power rails, GPIOs (reset/enable), and a flash/strobe driver feed the sensor side.
Below, each block is explained from the BSP angle.

---

## 2. Lens and IR-cut filter

- **Lens:** purely optical; no driver. Its focal length and aperture are module
  properties. Bring-up concern: make sure it's the right module variant (wide vs
  tele) so your tuning matches.
- **IR-cut:** on day/night cameras it is a mechanical filter swung by a small motor
  or solenoid, sometimes GPIO-controlled. If present it shows up as a GPIO your BSP
  code or a small platform driver toggles.

---

## 3. CMOS sensor (the heart)

The sensor is an I2C slave that:
- Receives a **master clock (XCLK/MCLK/INCK)**, typically 6–27 MHz, from the SoC or a
  dedicated oscillator. Its internal **PLL** multiplies this up to the pixel clock.
- Receives **power** on multiple rails (see §8): analog (AVDD ~2.8V), digital core
  (DVDD ~1.2V), and I/O (DOVDD ~1.8V).
- Receives a **reset (XCLR/RESETB)** and sometimes a **power-down (PWDN)** GPIO.
- Is configured over **I2C/CCI** by writing register tables.
- Streams **RAW Bayer** out over **MIPI CSI-2** D-PHY/C-PHY lanes.

Driver: `drivers/media/i2c/<sensor>.c` (e.g. `imx219.c`, `ov5640.c`). Covered in
depth in Module 9.

---

## 4. EEPROM / OTP (calibration store)

Camera modules embed an **EEPROM** (or sensor OTP) holding per-module calibration:
lens shading correction (LSC) maps, AWB calibration, autofocus VCM range, serial
number. It's a separate I2C slave.

```
 Userspace 3A/tuning reads EEPROM → applies LSC/AWB calibration → ISP
```

In Linux it can be a generic `at24` EEPROM device or a dedicated sensor-specific
driver. The kernel usually just exposes it; tuning happens in userspace/HAL.

---

## 5. VCM — voice coil motor (autofocus actuator)

A current-driven coil moves the lens. Driven by a tiny I2C DAC (DW9714, AK7375,
DW9807). Linux models it as a **lens sub-device**:

```
drivers/media/i2c/dw9714.c   → exposes V4L2_CID_FOCUS_ABSOLUTE
```

The DAC code maps monotonically from infinity focus (0) to macro (max). AF algorithm
sweeps it. Bring-up concern: get the I2C address and the code-to-distance polarity
right, and supply VAF (VCM power rail).

---

## 6. Flash / strobe / torch

An LED flash driver (e.g. LM3560, drivers/leds/flash) provides torch (continuous)
and flash (timed strobe synchronized to frame capture). The sensor exposes a
**FSTROBE** output to trigger the flash exactly during exposure. In Linux this is the
**V4L2 flash sub-device** API (`V4L2_CID_FLASH_*`). Important for low-light capture.

---

## 7. Serializer / deserializer (automotive / long cable)

When the sensor is far from the SoC (cars, industrial), MIPI's short reach is solved
by a **SerDes** pair:

```
 Sensor ─MIPI─► SERIALIZER ─(coax/STP, GMSL2 or FPD-Link III)─► DESERIALIZER ─MIPI─► SoC
            (e.g. MAX9295)        up to 15 m                    (e.g. MAX9296)
```

- **GMSL (Maxim/ADI)** and **FPD-Link III (TI)** are the two dominant standards.
- The deserializer aggregates multiple cameras onto one CSI port using **MIPI virtual
  channels**, plus tunnels the I2C control back to each sensor.
- Linux drivers: `drivers/media/i2c/max9286.c` (deserializer), `max9271`/`max96717`
  (serializer). They appear as additional media-controller sub-devices in the graph.

This is hugely relevant to **automotive BSP** work: the I2C addresses are remapped by
the SerDes, link-locking and back-channel setup are common bring-up failures.

---

## 8. Power rails and the power sequence

A typical sensor needs three+ rails plus the VCM rail, brought up in a strict order:

```
Rail      Typical V   Purpose
─────────────────────────────────────────────
DOVDD     1.8 V       Digital I/O (I2C, MIPI)
AVDD      2.8 V       Analog (pixel array, ADC)
DVDD      1.2 V       Digital core / PLL
VAF       2.8 V       VCM (autofocus) — optional

Power-up order (datasheet-defined), e.g.:
  1. Enable DOVDD
  2. Enable AVDD
  3. Enable DVDD
  4. Start XCLK (master clock)
  5. Wait t1 (e.g. >1 ms)
  6. De-assert RESETB / XCLR  (release reset)
  7. Wait t2 before first I2C access
```

Getting the **order and delays wrong** is the #1 sensor bring-up failure: the sensor
either doesn't ACK on I2C or streams garbage. In Linux this sequence lives in the
sensor driver's `power_on()` / runtime-PM resume path using the **regulator**,
**clk**, and **gpiod** frameworks (Module 9).

```c
ret = regulator_bulk_enable(SENSOR_NUM_SUPPLIES, sensor->supplies);
clk_prepare_enable(sensor->xclk);
gpiod_set_value_cansleep(sensor->reset_gpio, 0);  /* release reset */
usleep_range(1000, 2000);
```

---

## 9. Clocks

```
 XCLK (e.g. 24 MHz) ─► Sensor internal PLL ─► pixel clock ─► MIPI bit clock
```

- The SoC provides XCLK from a camera clock controller (CCC) or PMIC.
- The sensor PLL config (in its register table) determines pixel clock, link
  frequency (`V4L2_CID_LINK_FREQ`), and thus achievable resolution/fps.
- The **MIPI D-PHY link frequency** must match between sensor output and the SoC
  CSIPHY config, or you get CRC/sync errors. This is a classic bring-up mismatch.

---

## 10. GPIO and reset

- **RESETB / XCLR:** active-low reset; released after power+clock stable.
- **PWDN / enable:** some sensors have a power-down pin instead of cutting rails.
- Modeled with `gpiod` (`reset-gpios`, `powerdown-gpios` in DT). Polarity bugs
  (active-high vs active-low) are common — `GPIO_ACTIVE_LOW` in DT must match the
  schematic.

---

## 11. I2C and CCI (the control bus)

The sensor is configured over I2C. On Qualcomm SoCs the camera I2C master is a
dedicated block called **CCI (Camera Control Interface)** inside CAMSS, not the
generic I2C controller — it's optimized for camera register bursts and runs at up to
1 MHz / Fast-mode-plus.

```
 SoC ── I2C/CCI ──► Sensor (0x10/0x20...)  write 16-bit reg, 8/16-bit data
                ──► EEPROM
                ──► VCM
```

Driver: Qualcomm `drivers/media/platform/qcom/camss/camss-cci` style, or generic
`i2c` adapters elsewhere. Register access helpers (`regmap_i2c`) are common.

---

## 12. The SoC camera subsystem (zoom-out)

Inside the SoC, the receive + process chain is:

```
 CSIPHY (D-PHY/C-PHY analog RX + deskew)
    │
 CSID / CSI-2 RX (packet decode, VC/DT demux, CRC/ECC check)
    │
 IFE / VFE / ISP (demosaic, 3A stats, scale, crop, format)
    │
 Write-Master / DMA (AXI) ──► SMMU ──► DDR frame buffer
```

- **Qualcomm:** CSIPHY → CSID → IFE (VFE) → BUS, all under **CAMSS** (Module 11).
- **NVIDIA:** NVCSI → VI → ISP (Module 12).
- **i.MX8:** MIPI CSI-2 RX → ISI / ISP.

Each of these is a media-controller **entity** with pads and links (Module 5).

---

## 13. DMA engine and DDR

The ISP's write-master is a DMA engine that pushes processed frames into DDR buffers
that **videobuf2** manages (Module 13). On systems with an **IOMMU/SMMU** (Module 14)
the DMA addresses are I/O virtual addresses translated by the SMMU, giving
scatter-gather and isolation. The bandwidth here (GB/s for 4K60) is the dominant
system-design constraint (Module 16).

---

## 14. Downstream consumers

Once a frame is in DDR it's shared (ideally zero-copy via **dma-buf**) to:
- **GPU** for preview composition,
- **Video encoder** for recording/streaming,
- **CPU / NN accelerator** for analytics (object detection, etc.),
- **Display** for live preview.

Zero-copy dma-buf sharing across these IPs is the whole point of the memory design in
Module 14.

---

## 15. Schematic-to-driver bring-up checklist (BSP cheat sheet)

```
[ ] Power rails wired & named correctly in DT (regulator-supply props)
[ ] Power-up order + delays match sensor datasheet
[ ] XCLK present, correct frequency (scope it!)
[ ] RESETB polarity correct (GPIO_ACTIVE_LOW vs HIGH)
[ ] Sensor ACKs on I2C/CCI at expected address
[ ] Chip-ID register reads expected value (first probe sanity check)
[ ] MIPI lane count & link-frequency match between sensor & CSIPHY
[ ] CSIPHY settle/deskew timing configured for the link freq
[ ] No CRC/ECC errors at CSID (check debugfs/status regs)
[ ] Frames land in DDR (vb2 buffers dequeue with correct size)
```

---

## 16. Interview Q&A

**Q1. A new sensor won't ACK on I2C during bring-up. First three things to check?**
(1) Power rails up and in the correct order/voltage; (2) XCLK actually present and at
the right frequency (scope it — many sensors need the clock before I2C works); (3)
RESETB released with correct polarity and after the required delay.

**Q2. What is CCI and why not just use the SoC's normal I2C controller?**
CCI (Camera Control Interface) is Qualcomm's dedicated camera I2C master inside CAMSS,
optimized for fast burst register writes and tightly integrated with the camera power
domain. Functionally it's I2C but tuned for camera control traffic.

**Q3. Sensor streams but the image is corrupted/striped. Where do you look first?**
Likely a MIPI link mismatch: lane count or link frequency differs between sensor
output and CSIPHY config, or CSIPHY settle timing is wrong for the data rate. Check
CSID CRC/ECC error counters; confirm `link-frequency` in DT matches the sensor PLL
setting.

**Q4. Why do automotive cameras use serializers/deserializers?**
MIPI CSI-2 only reaches a few centimeters. SerDes (GMSL2, FPD-Link III) tunnel MIPI +
I2C over coax/STP up to ~15 m and aggregate multiple cameras onto one CSI port via
virtual channels, with link locking and an I2C back-channel.

**Q5. What does the EEPROM on a camera module store and who uses it?**
Per-module calibration: lens-shading maps, AWB calibration, AF/VCM range, serial
number. Userspace/HAL tuning reads it and programs the ISP; the kernel typically just
exposes the EEPROM as an I2C device.

**Q6. Describe the role of the master clock (XCLK) vs the pixel clock.**
XCLK is the external reference (e.g. 24 MHz) fed to the sensor; the sensor's internal
PLL multiplies it to the pixel clock and MIPI bit clock. Resolution/fps and link
frequency derive from the PLL config, not directly from XCLK.

**Q7. Where does the SMMU sit and why does it matter for camera?**
Between the ISP write-master (DMA) and DDR. It translates device I/O virtual addresses
to physical, enabling scatter-gather (non-contiguous buffers look contiguous to the
IP) and memory isolation/protection between camera and other clients.

---

### Key takeaways
- A camera module is a small system: sensor + EEPROM + VCM + clock + multiple rails +
  reset/enable GPIOs, all on I2C/CCI, streaming RAW over MIPI.
- Power sequencing (order + delays) is the #1 bring-up failure; it lives in the
  sensor driver's power_on/runtime-PM path.
- SerDes extends MIPI for automotive and aggregates cameras via virtual channels.
- The SoC chain CSIPHY→CSID→ISP→DMA→DDR is the subject of Modules 8–14.
