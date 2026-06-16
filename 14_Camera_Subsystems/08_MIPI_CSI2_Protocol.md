# Module 8 — MIPI CSI-2 Protocol

> **Scope:** The MIPI CSI-2 (Camera Serial Interface 2) protocol that carries pixel
> data from sensor to SoC. Covers the physical layer (D-PHY and C-PHY), lane model,
> the packet structure (short/long packets, data types, virtual channels, ECC, CRC),
> the frame/line synchronization signaling, lane calibration, and how all of this maps
> to the CSIPHY/CSID hardware you configure and debug. This is the transport layer
> beneath every camera pipeline.

---

## 1. Why CSI-2 and where it sits

CSI-2 is the standard high-speed serial link between a camera sensor and the SoC
receiver. It replaced slow, wide parallel interfaces with a few differential serial
lanes.

```
 Sensor                        SoC
 ┌────────┐  D-PHY/C-PHY lanes ┌─────────┐  ┌──────┐  ┌─────┐
 │ pixel  │  ═══════════════►  │ CSIPHY  │─►│ CSID │─►│ ISP │
 │ array  │  clk + data lanes  │ (RX PHY)│  │(decode)│ │     │
 └────────┘                    └─────────┘  └──────┘  └─────┘
   protocol+PHY layers          analog RX   packet     pixel
                                            decode     processing
```

- **CSIPHY** = the analog receiver (deserializes the differential signal, handles
  clocking/deskew). Configured for lane count + link frequency.
- **CSID** (CSI Decoder) = parses packets, checks ECC/CRC, demuxes by virtual
  channel/data type, and forwards pixel streams to the ISP.

You program both from the kernel; mismatches here are the #1 cause of "garbled image"
or "no frames" bugs.

---

## 2. The layered model

```
 ┌─────────────────────────────────────────┐
 │ Application layer (pixel data, formats)   │  RAW10, YUV422, embedded data
 ├─────────────────────────────────────────┤
 │ Protocol layer                            │  packets, VC, data type, ECC/CRC
 │   - Pixel-to-Byte packing                 │
 │   - Low-Level Protocol (packet framing)   │
 │   - Lane Management (distribute/merge)    │
 ├─────────────────────────────────────────┤
 │ PHY layer (D-PHY or C-PHY)                │  HS/LP states, clock, electrical
 └─────────────────────────────────────────┘
```

---

## 3. Physical layer: D-PHY

D-PHY is the most common camera PHY. Key facts:

```
 D-PHY uses:
   1 clock lane  (differential, source-synchronous DDR clock)
   1..4 data lanes (differential)

 Each lane has two operating modes:
   LP (Low Power) : single-ended, ~1.2V, slow — used for control/sync/idle
   HS (High Speed): differential, ~200mV swing — used for actual data burst

 Data rate per lane: up to 2.5 Gbps (D-PHY v1.2), higher in later versions.
 Total = data_rate_per_lane × number_of_lanes.
```

```
 Lane state sequence per HS burst:
   LP-11 (idle) → LP-01 → LP-00 → HS-0 (SoT) → [HS data...] → EoT → LP-11
                          ▲                                     ▲
                     HS entry                              HS exit
```

The **clock lane** provides a DDR clock; data is sampled on both edges. The
**link/bit clock** = `link_frequency` from DT (Module 7) × 2 (DDR) gives the bit rate
per lane. The CSIPHY must be told the link frequency so it sets the right **HS settle
time** (`T_HS_SETTLE`) — the window to ignore after HS entry before sampling. Wrong
settle = bit errors / CRC failures. This is *the* classic CSI tuning parameter.

---

## 4. Physical layer: C-PHY

C-PHY is the newer, higher-efficiency alternative:

```
 D-PHY:  2 wires per lane (differential) + separate clock lane
 C-PHY:  3 wires per "trio" (lane), clock embedded in the signaling
         Uses 3-phase symbol encoding → ~2.28 bits/symbol
         No separate clock lane → more data throughput per wire
```

C-PHY encodes the clock into the data using 3-phase symbols across a 3-wire trio,
achieving higher bandwidth per wire-count. The sensor and CSIPHY must agree D-PHY vs
C-PHY (`bus-type` in DT, Module 7). Most embedded designs still use D-PHY; high-end
phone sensors increasingly use C-PHY.

---

## 5. Lanes and lane distribution

With multiple data lanes, bytes are **distributed round-robin** across lanes by the
transmitter and **merged** by the receiver:

```
 Byte stream: B0 B1 B2 B3 B4 B5 B6 B7 ...
 4 lanes:
   Lane0: B0 B4 B8 ...
   Lane1: B1 B5 B9 ...
   Lane2: B2 B6 ...
   Lane3: B3 B7 ...
```

More lanes = more bandwidth = higher resolution/fps. The `data-lanes = <1 2 3 4>` DT
property maps the logical lanes to physical lanes (and can reorder for board routing).
**Lane count must match on sensor and CSIPHY** (Module 7) or the byte de-interleaving
is wrong → corrupted image.

---

## 6. Packet structure

CSI-2 carries two packet kinds: **short** (sync/control) and **long** (pixel data).

```
 SHORT PACKET (4 bytes) — frame/line synchronization:
 ┌────────┬──────────────┬─────┐
 │ DI (1) │ Word Count(2) │ ECC │
 └────────┴──────────────┴─────┘
   DI = Data Identifier = [VC(2 bits) | Data Type(6 bits)]
   For sync packets, WC field carries the frame number.

 LONG PACKET (variable) — pixel line:
 ┌────────┬──────────────┬─────┬──────────── payload ───────────┬───────┐
 │ DI (1) │ Word Count(2) │ ECC │   pixel data (WC bytes)        │ CRC(2)│
 └────────┴──────────────┴─────┴────────────────────────────────┴───────┘
   └────────── Packet Header (PH) ───────┘                       Packet Footer
```

- **DI (Data Identifier):** 1 byte = Virtual Channel (2 bits) + Data Type (6 bits).
- **Word Count (WC):** payload length in bytes (one image line for long packets).
- **ECC:** Hamming code over the packet header — corrects 1-bit, detects 2-bit header
  errors.
- **CRC:** 16-bit CRC over the payload — detects corrupted pixel data.

---

## 7. Virtual Channels (VC) and Data Types (DT)

### 7.1 Virtual Channel
The 2-bit (extendable to more) VC lets **multiple streams share one physical link**:

```
 One CSI link carrying 2 cameras (via SerDes) or HDR exposures:
   VC0 → camera A / long exposure
   VC1 → camera B / short exposure
 CSID demuxes by VC into separate pipelines.
```

This is how SerDes aggregation (Module 2) and stagger-HDR (Module 1/4) put multiple
logical streams on one link. The CSID routes each VC to a different ISP input.

### 7.2 Data Type
The 6-bit DT identifies the payload format:

```
 DT (hex)   Format
 ────────────────────────────────
 0x00-0x07  Sync short packets (FS, FE, LS, LE)
 0x12       Embedded 8-bit non-image data (sensor metadata)
 0x18-0x1F  YUV (e.g. 0x1E YUV422-8)
 0x22       RGB565
 0x24       RGB888
 0x28       RAW6
 0x2A       RAW8
 0x2B       RAW10
 0x2C       RAW12
 0x2D       RAW14
```

The DT must match what the sensor outputs (Module 3 §10) and what the CSID is told to
expect. A DT mismatch → CSID drops the packets or misinterprets the line length.

---

## 8. Frame and line synchronization

Sync is signaled by **short packets** with special data types:

```
 Frame Start (FS, DT 0x00)  ─┐
   Line Start (LS) [optional] │
     [long packet: line 0]    │
   Line End (LE)  [optional]  │  one frame
     [long packet: line 1]    │
       ...                    │
 Frame End (FE, DT 0x01)    ──┘
```

```
 Timeline of one frame on the link:
 FS ── line0 ── line1 ── line2 ── ... ── lineN ── FE
 │                                                 │
 ▲ CSID raises SOF (start-of-frame) event     ▲ EOF event → buffer done
```

- The CSID detects **FS** → generates a Start-of-Frame interrupt/event
  (`V4L2_EVENT_FRAME_SYNC`, Module 6) used for 3A timing.
- **FE** → End-of-Frame → the ISP write-master completes the buffer → vb2 marks it
  done (Module 13).
- **FS without matching FE**, or wrong line count, indicates a link/timing problem.

---

## 9. Lane calibration, deskew, and settle timing

At high data rates, lane-to-lane skew and the HS settle window must be calibrated:

```
 T_HS_SETTLE : time to wait after HS entry before sampling data
               (must be set per link frequency in the CSIPHY)
 Deskew      : (high-speed D-PHY ≥1.5 Gbps) periodic deskew bursts align lanes
 C-PHY calib : symbol-rate calibration sequences
```

The CSIPHY driver computes settle timing from `V4L2_CID_LINK_FREQ` (which the sensor
advertises, Module 3/9). On Qualcomm this is the CSIPHY's settle-count register; on
many SoCs there's a formula like:

```c
/* simplified: settle count derived from link freq and a reference clock */
settle = (T_HS_SETTLE_ns * csiphy_clk_hz) / 1e9;
```

Getting settle wrong is the most common "intermittent CRC errors / works at low res,
fails at high res" bug.

---

## 10. Error detection: ECC and CRC

```
 Packet Header  → protected by ECC (Hamming):
     - corrects single-bit errors in the header
     - detects (uncorrectable) double-bit errors
 Packet Payload → protected by CRC-16:
     - detects corrupted pixel data (no correction)
```

The CSID exposes **error counters** (ECC errors, CRC errors, unmatched-VC, overflow).
These are your primary CSI-health telemetry:

```
 High CRC count        → PHY settle/deskew wrong, signal integrity, wrong link freq
 ECC errors            → header corruption, similar PHY causes
 FIFO/buffer overflow  → downstream (ISP/DMA) not keeping up → backpressure
 VC mismatch           → CSID routing config doesn't match sensor's VC usage
```

---

## 11. The CSIPHY → CSID receive flow in the kernel

```
 1. Sensor s_stream(1): sensor starts emitting FS/lines/FE on CSI lanes
 2. CSIPHY: powered, lane count + settle timing programmed from link_freq
            → locks onto HS clock, deserializes lanes
 3. CSID: configured with expected VC + DT + line width
            → validates ECC/CRC, demuxes VC/DT
            → forwards pixel stream to ISP input (or directly to DMA in RDI mode)
 4. CSID FS interrupt → SOF event; FE interrupt → frame complete
 5. Error interrupts → increment CRC/ECC/overflow counters (debugfs/dmesg)
```

On Qualcomm, "RDI" (Raw Dump Interface) mode lets the CSID write RAW straight to DDR
bypassing the IFE — used for RAW capture and debugging (Module 11).

---

## 12. Bandwidth math (essential for system design)

```
 Required link rate = width × height × fps × bits_per_pixel × overhead
                      ─────────────────────────────────────────────────
                                  number_of_lanes

 Example: 1920×1080 @ 60fps, RAW10:
   pixels/s = 1920 × 1080 × 60 ≈ 124.4 Mpix/s
   bits/s   = 124.4M × 10      ≈ 1.244 Gbps  (+ ~ blanking/packet overhead)
   On 2 lanes @ ~700 Mbps/lane → fits; on 1 lane it would not.

 4K @ 30fps RAW12:
   3840×2160×30×12 ≈ 2.98 Gbps → needs ≥2-4 lanes depending on per-lane rate.
```

This calculation drives lane-count and link-frequency choices (Module 7) and DDR/QoS
budgeting (Module 16).

---

## 13. Debugging CSI-2

```bash
# CSID error/status counters (vendor-specific debugfs)
cat /sys/kernel/debug/.../csid*/status        # CRC/ECC/overflow counts
dmesg | grep -iE "csid|csiphy|crc|ecc|overflow|sof|fifo"

# Confirm lanes/link-freq agree across the graph
media-ctl -p -d /dev/media0                    # shows pad formats / bus config

# Hardware-level:
#   - Logic analyzer / MIPI protocol analyzer on the CSI lanes:
#       verify FS/FE, data types, lane count, byte alignment
#   - Oscilloscope: check HS differential swing, clock, LP/HS transitions
#   - Scope the link bit rate vs expected (link_freq × 2 for DDR)
```

Symptom → cause table:
```
 No frames at all              → CSIPHY not locking (no clock lane, wrong lanes),
                                  sensor not streaming, settle way off
 Intermittent CRC, worse at hi-res → settle/deskew timing wrong for link freq
 Image shifted/split           → wrong lane mapping (data-lanes order)
 Garbled colors / wrong size   → wrong data type / word count vs format
 Two frames per sensor frame   → HDR/multi-VC (expected), or CSID VC misroute
 FIFO overflow errors          → ISP/DMA backpressure (DDR bandwidth, Module 16)
```

---

## 14. Interview Q&A

**Q1. Describe the CSI-2 long packet structure.**
Packet Header = Data Identifier (VC + Data Type), Word Count (payload bytes), and ECC.
Then the payload (one image line, WC bytes), followed by a Packet Footer with a 16-bit
CRC. ECC protects the header; CRC protects the payload.

**Q2. What is a virtual channel and why is it useful?**
A 2-bit (extendable) field in the Data Identifier that multiplexes multiple logical
streams onto one physical CSI link — e.g. two cameras over a SerDes, or long/short HDR
exposures. The CSID demuxes by VC into separate pipelines.

**Q3. D-PHY vs C-PHY?**
D-PHY uses 2 wires per data lane plus a separate clock lane, LP/HS signaling, up to
~2.5 Gbps/lane. C-PHY uses a 3-wire trio with an embedded clock and 3-phase symbol
encoding (~2.28 bits/symbol), giving more throughput per wire and no separate clock
lane. Both ends must agree (DT `bus-type`).

**Q4. You get intermittent CRC errors that worsen at higher resolution. Cause?**
Almost always CSIPHY HS settle (T_HS_SETTLE) or deskew timing wrong for the link
frequency. Higher resolution = higher data rate = tighter timing, so a marginal settle
value that passes at low rates fails at high rates. Recompute settle from
`link-frequency`.

**Q5. What do ECC and CRC each cover and what's the difference?**
ECC (Hamming) protects the packet header — it corrects single-bit and detects
double-bit header errors. CRC-16 protects the payload — it only detects corrupted pixel
data, no correction. Header integrity is critical because WC/DT errors desync the whole
frame.

**Q6. How does the SoC know a frame started and ended?**
Frame Start (DT 0x00) and Frame End (DT 0x01) short packets. The CSID detects FS →
raises a start-of-frame event/interrupt; FE → end-of-frame, completing the DMA buffer.

**Q7. How is link bandwidth calculated and why does it matter?**
`rate = width × height × fps × bpp / lanes` (plus overhead). It determines the required
per-lane data rate, lane count, and link frequency, and feeds DDR/QoS budgeting. If the
link can't carry the rate you must reduce resolution/fps/bit-depth or add lanes.

**Q8. What does a CSID FIFO/buffer overflow indicate?**
Downstream backpressure — the ISP/DMA can't drain pixels fast enough (often DDR
bandwidth/QoS starvation), so the CSID's input FIFO overflows and drops data. It points
to a system-bandwidth problem, not a PHY/link problem.

---

### Key takeaways
- CSI-2 = packets (short for FS/FE/LS/LE sync, long for pixel lines) over D-PHY or
  C-PHY lanes; DI carries Virtual Channel + Data Type.
- CSIPHY deserializes (lane count + HS settle from link frequency); CSID validates
  ECC/CRC and demuxes VC/DT to the ISP.
- Lane count, link frequency, and data type must match end-to-end (sensor↔DT↔CSIPHY)
  or you get CRC errors / garbled frames.
- Settle/deskew timing is the classic high-rate CSI bug; CSID error counters are your
  primary CSI-health telemetry.
