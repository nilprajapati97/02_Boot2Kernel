# Module 15 — Camera Debugging

> **Scope:** The complete camera debugging toolbox for a kernel/BSP engineer. Covers the
> userspace tools (`media-ctl`, `v4l2-ctl`, `yavta`), kernel facilities (dynamic debug,
> debugfs, ftrace/tracepoints, dev_dbg), the layer-by-layer isolation methodology
> (sensor → CSI → ISP → DMA → userspace), and hardware tools (logic/protocol analyzer,
> oscilloscope). Ends with a symptom→root-cause matrix and a structured triage flow.
> This module operationalizes everything in Modules 1–14.

---

## 1. The golden rule: isolate the layer first

A camera pipeline has many layers; the first job is to **localize** the failure, not
guess. Walk the pipeline and prove each stage:

```
 SENSOR ──► CSIPHY ──► CSID ──► ISP ──► DMA/vb2 ──► V4L2 node ──► app
   │          │          │        │         │            │
 chip-ID    locks?     CRC ok?  RAW ok?  buffers     DQBUF
 reads?     settle?    VC/DT?   YUV ok?  done?       returns?

 Test each boundary independently. Most bugs are at ONE boundary.
```

Two highest-value isolation techniques (from earlier modules):
- **Capture RAW from the CSID bypass / RDI path** (Module 10/11): good RAW + bad YUV →
  ISP problem; bad RAW → sensor/CSI problem.
- **Read the sensor chip-ID** (Module 9): fails → power/clock/reset/I2C problem.

---

## 2. media-ctl — topology and routing

```bash
media-ctl -p -d /dev/media0                 # full topology + current pad formats + links
media-ctl --print-dot -d /dev/media0 | dot -Tpng -o cam.png   # visual graph

# enable a link (route)
media-ctl -d /dev/media0 -l '"imx219 4-0010":0 -> "msm_csid0":0[1]'

# set pad formats end-to-end (must be consistent or STREAMON → -EPIPE)
media-ctl -d /dev/media0 -V '"imx219 4-0010":0 [fmt:SRGGB10/1920x1080]'
media-ctl -d /dev/media0 -V '"msm_csid0":1     [fmt:SRGGB10/1920x1080]'
```

First command in any camera debug session. Reveals: missing links, format mismatches,
which video node corresponds to which sensor.

---

## 3. v4l2-ctl — formats, controls, capture

```bash
v4l2-ctl --all -d /dev/video0                       # caps + current format + controls
v4l2-ctl --list-formats-ext -d /dev/video0          # all pixfmts/sizes/fps
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls           # subdev controls + ranges
v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl exposure=2000,analogue_gain=128

# capture smoke test (counts frames, reports fps)
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
         --stream-mmap --stream-count=100

# capture to file for inspection
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10 --stream-to=frame.raw

# enable V4L2 core ioctl tracing in dmesg
echo 0x3 > /sys/class/video4linux/video0/dev_debug
```

`--stream-to` + a viewer (`ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080
frame.raw`) lets you eyeball artifacts and map them to ISP stages (Module 4 §8).

---

## 4. yavta — fine-grained buffer/streaming control

```bash
# request 4 buffers, capture 100 frames of RAW10
yavta -n 4 -c100 -f SRGGB10 -s 1920x1080 /dev/video0

# capture and write frames to disk
yavta -n 4 -c10 -F/tmp/frame#.raw -f SRGGB10 -s 1920x1080 /dev/video0

# set a control while streaming
yavta --set-control '0x00980911 2000' /dev/v4l-subdev0   # exposure
```

`yavta` exposes vb2 mechanics (buffer counts, memory type, userptr/dmabuf) more directly
than `v4l2-ctl` — useful for buffer/DMA debugging (Module 13).

---

## 5. Kernel: dynamic debug (dev_dbg without rebuilding)

```bash
# turn on all dev_dbg in a module
echo 'module imx219 +p'  > /sys/kernel/debug/dynamic_debug/control
echo 'module qcom_camss +p' > /sys/kernel/debug/dynamic_debug/control

# target a single file or function
echo 'file rkisp1-isp.c +p' > /sys/kernel/debug/dynamic_debug/control
echo 'func csid_isr +p'      > /sys/kernel/debug/dynamic_debug/control

# include line/function/module in output
echo 'module imx219 +pflmt' > /sys/kernel/debug/dynamic_debug/control
dmesg -w    # watch live
```

Dynamic debug turns `dev_dbg()`/`pr_debug()` on at runtime — essential for tracing
probe, power sequencing, register writes, and IRQ paths without a custom build.

---

## 6. Kernel: debugfs (vendor status/error counters)

```bash
# generic vb2 buffer state debug
echo 3 > /sys/module/videobuf2_common/parameters/debug

# vendor camera debugfs (paths vary by SoC)
ls /sys/kernel/debug/                         # discover what's exposed
cat /sys/kernel/debug/camss*/...              # Qualcomm CSID/VFE counters (where present)
cat /sys/kernel/debug/.../csid*/status        # CRC/ECC/overflow counters (Module 8)

# devices stuck in probe deferral
cat /sys/kernel/debug/devices_deferred        # Module 7 EPROBE_DEFER debugging

# clocks / regulators (is the sensor actually powered/clocked?)
cat /sys/kernel/debug/clk/clk_summary | grep -i cam
cat /sys/kernel/debug/regulator/regulator_summary
```

CSID error counters (CRC/ECC/overflow) are your primary CSI-health telemetry. Rising CRC
= PHY/settle problem (Module 8); rising overflow = bandwidth/backpressure (Module 16).

---

## 7. Kernel: ftrace and tracepoints

```bash
cd /sys/kernel/debug/tracing

# V4L2 has tracepoints for QBUF/DQBUF
echo 1 > events/v4l2/enable
echo 1 > events/vb2/enable
cat trace_pipe        # live stream of buffer events with timestamps

# trace a specific function graph (e.g. the IRQ handler latency)
echo function_graph > current_tracer
echo csid_isr > set_graph_function
echo 1 > tracing_on; cat trace_pipe

# measure frame interval jitter from vb2 timestamps
echo 'p:dqbuf vb2_buffer_done' > kprobe_events   # example kprobe
```

```
 Use ftrace to answer:
   - Are frames arriving at the expected fps? (timestamp deltas in vb2 events)
   - Is there IRQ latency / a slow handler causing drops?
   - Where does time go between SOF and buffer-done?
```

Tracepoints + `trace_pipe` give a timestamped event log of the buffer pipeline — ideal
for **frame-drop** and **latency** analysis (Module 16).

---

## 8. The structured triage flow

```
 No /dev/videoN or /dev/mediaN?
   └► DT / probe problem. dmesg | grep EPROBE_DEFER; devices_deferred; check DT (Mod 7)

 media-ctl -p shows entities but no links / format mismatch?
   └► route with -l, set pad formats with -V (Module 5). STREAMON -EPIPE = format chain.

 Sensor subdev present but chip-ID failed at probe?
   └► power seq/order, XCLK, reset polarity, I2C addr (Module 2/9). Scope XCLK & I2C.

 STREAMON ok but DQBUF blocks forever (no frames)?
   ├► CSID CRC/overflow counters rising? → CSIPHY settle / link-freq mismatch (Mod 8)
   ├► no IRQs at all? → sensor not streaming (s_stream), CSIPHY not locking
   └► IRQs but buffers not done? → write-master DMA addr not programmed (Mod 13)

 Frames arrive but image is wrong?
   ├► capture RAW (RDI/CSID bypass): RAW bad → sensor/CSI; RAW good → ISP (Mod 10)
   ├► false color / green → Bayer phase / bus code (Module 3)
   ├► sheared/diagonal → bytesperline/stride wrong (Module 6/13)
   ├► stale/garbage but addr ok → cache coherency (Module 13/14)
   └► color cast / brightness → AWB/AE in ISP (Module 4)

 Frames drop intermittently / can't hit fps?
   └► bandwidth/QoS: CSID overflow, IFE overflow, DDR/EMC clock, CMA stalls (Mod 16)

 SMMU context fault in dmesg?
   └► decode iova + context bank → wrong stream IDs / unmapped buffer (Module 14)
```

---

## 9. Hardware tools (when software isn't enough)

```
 OSCILLOSCOPE:
   - XCLK present and at the right frequency (sensor won't I2C without it)
   - Power rails sequencing and voltage levels (Module 2 §8)
   - RESETB timing/polarity
   - MIPI HS differential swing (~200 mV), LP levels, clock lane toggling

 LOGIC ANALYZER (low-speed):
   - I2C/CCI transactions: does the sensor ACK? are register writes correct?
   - GPIO reset/enable timing

 MIPI PROTOCOL ANALYZER (high-speed, e.g. capture card / specialized):
   - Decode CSI-2 packets: FS/FE/LS/LE, data types, virtual channels, lane count
   - Verify word counts vs expected line length
   - Catch CRC/ECC errors at the wire level (Module 8)
   - Confirm HDR multi-VC / stagger streams
```

```
 Decision: when to reach for hardware tools
   - Sensor never ACKs on I2C       → scope XCLK + rails + reset (likely power/clock)
   - CSID never sees packets         → MIPI analyzer: is the sensor transmitting at all?
   - Intermittent CRC at high rate   → scope MIPI signal integrity; analyzer for errors
   - "Works on one board, not another"→ hardware/layout: scope the actual signals
```

---

## 10. Worked example: "DQBUF hangs, no frames"

```
 1. media-ctl -p
      → links enabled? formats consistent? (else fix with -l / -V)
 2. dmesg | grep -iE "csid|csiphy|crc|overflow|sof"
      → SOF interrupts firing? CRC errors?
        - No SOF at all → sensor not streaming OR CSIPHY not locking
        - SOF + CRC rising → settle/link-freq mismatch (Module 8)
 3. v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls
      → is link_freq advertised correctly? (drives CSIPHY settle)
 4. echo 'module qcom_camss +p' > .../dynamic_debug/control
      → trace s_stream chain: did sensor s_stream(1) run? did write-master get an addr?
 5. If sensor s_stream ran but no MIPI activity → scope/analyze the CSI lanes
 6. If MIPI ok but buffers not completing → write-master DMA addr / IRQ (Module 13)
```

This sequence converts "it doesn't work" into a specific failing boundary in minutes.

---

## 11. Interview Q&A

**Q1. A customer says "camera shows a black screen." How do you triage?**
Isolate the layer: confirm `/dev/video*`/`media0` exist (probe/DT ok); `media-ctl -p`
for links/formats; check sensor chip-ID and that `s_stream` ran; look for SOF interrupts
and CSID CRC/overflow counters; capture RAW via RDI/CSID bypass to split sensor/CSI from
ISP. Black + frames arriving → ISP/exposure (AE); no frames → sensor/CSI/CSIPHY.

**Q2. How do you turn on driver debug prints without rebuilding the kernel?**
Dynamic debug: `echo 'module <name> +p' > /sys/kernel/debug/dynamic_debug/control`
(optionally `+pflmt` for file/line/func), then watch `dmesg -w`. It enables
`dev_dbg`/`pr_debug` at runtime per module/file/function.

**Q3. What's the single most useful isolation technique for "image looks wrong"?**
Capture RAW from the CSID bypass / RDI path. If the RAW is correct, the sensor and CSI
transport are fine and the bug is in the ISP configuration; if the RAW is wrong, it's
sensor/CSI. This split eliminates half the pipeline immediately.

**Q4. You suspect frame drops. Which tools quantify it?**
ftrace V4L2/vb2 tracepoints (`events/vb2/enable`, `trace_pipe`) give timestamped
buffer-done events — compute interval jitter and missed frames. CSID/IFE overflow
counters in debugfs and dmesg confirm whether drops are from bandwidth backpressure
(Module 16). `v4l2-ctl --stream-mmap` reports measured fps.

**Q5. CSID CRC error counter is climbing. Root cause and next step?**
A PHY/link-integrity problem: CSIPHY HS settle timing wrong for the link frequency, a
lane-count/link-freq mismatch (DT vs sensor), or signal integrity. Recompute settle from
the advertised `V4L2_CID_LINK_FREQ`, verify `data-lanes`/`link-frequencies` match on both
endpoints, and if needed put a MIPI analyzer/scope on the lanes (Module 8).

**Q6. When do you stop debugging in software and pick up a scope?**
When the failure is below the kernel's visibility: sensor never ACKs on I2C (scope XCLK,
rails, reset), CSID never sees any packets (MIPI analyzer to check the sensor is actually
transmitting), or board-specific "works here not there" issues that point to signal
integrity/layout.

**Q7. `cat /sys/kernel/debug/devices_deferred` shows the sensor. What does that mean?**
The sensor's probe keeps returning `-EPROBE_DEFER` because a dependency isn't ready —
usually a clock/regulator/GPIO provider or the remote CSI endpoint isn't available, or a
DT name is wrong so `devm_*_get` never succeeds. Cross-check the DT resource names and
the OF graph phandles (Module 7).

**Q8. How do you confirm the sensor is actually powered and clocked from the kernel?**
`cat /sys/kernel/debug/clk/clk_summary | grep -i cam` to see XCLK enabled/at rate, and
`/sys/kernel/debug/regulator/regulator_summary` to see the supplies enabled at the right
voltage. If those look right but the chip-ID still fails, scope the actual XCLK pin and
reset line at the sensor.

---

### Key takeaways
- Debug by isolating the failing boundary (sensor→CSI→ISP→DMA→node), not by guessing;
  RAW-via-RDI and chip-ID reads split the pipeline fast.
- `media-ctl -p` (topology/format), `v4l2-ctl` (capture/controls), and `yavta` (buffers)
  are the userspace core; dynamic debug, debugfs counters, and ftrace tracepoints are the
  kernel core.
- CSID CRC/overflow counters distinguish PHY problems (CRC) from bandwidth problems
  (overflow); ftrace quantifies frame drops/latency.
- Reach for scope/logic/MIPI analyzers when the failure is below kernel visibility (I2C
  no-ACK, no MIPI packets, signal integrity at high rates).
