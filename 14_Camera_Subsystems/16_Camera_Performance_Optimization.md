# Module 16 — Camera Performance Optimization

> **Scope:** Making the camera pipeline meet its latency, frame-rate, and bandwidth
> targets. Covers the bandwidth/DDR/QoS budget, where latency comes from and how to cut
> it, DMA and cache optimization, ISP throughput, multi-camera scaling, power/thermal
> tradeoffs, and the measurement methodology. This is the system-design module that ties
> Modules 8 (CSI bandwidth), 13/14 (buffers/DMA), and 11/12 (SoC) into "why is it
> dropping frames / too slow / too hot."

---

## 1. The three performance axes

```
 LATENCY    : time from photons → frame available to the app (glass-to-glass)
 THROUGHPUT : sustained frame rate (fps) at a given resolution
 BANDWIDTH  : bytes/s moved across CSI, ISP, and DDR — the usual bottleneck
              + POWER/THERMAL as the constraint that caps all three
```

Almost every camera performance bug is ultimately a **bandwidth or scheduling** problem
surfacing as dropped frames, missed fps, or overflow interrupts.

---

## 2. Bandwidth budgeting (the core skill)

### 2.1 CSI link bandwidth (Module 8 recap)
```
 link_rate = width × height × fps × bpp / lanes   (+ ~10-20% packet/blanking overhead)
 1080p60 RAW10  ≈ 1.24 Gbps   ;  4K30 RAW12 ≈ 2.98 Gbps
```

### 2.2 DDR bandwidth — the real bottleneck
Each frame is written and read from DDR **multiple times**:

```
 DDR traffic per frame (a 4K NV12 example, ~12 MB/frame @ 30fps ≈ 373 MB/s per pass):
   IFE write (RAW or YUV) ........... 1 write pass
   ISP temporal NR reference read ... 1 read pass (TNR, Module 4)
   ISP write processed .............. 1 write pass
   encoder read ..................... 1 read pass
   display/GPU read ................. 1 read pass
   stats write/read ................. small
   ─────────────────────────────────────────────
   Effective DDR BW = frame_size × fps × (number_of_passes)
```

```
 Total DDR BW = Σ (each IP's read + write passes) × frame_size × fps
              + every other DDR client (CPU, GPU, display, modem...)
 If this exceeds the DDR controller's capacity at the current EMC/DDR clock →
 QoS starvation → camera write-master can't drain → OVERFLOW → dropped frames.
```

This multi-pass accounting is *the* camera system-design interview question. The fix is
usually: raise DDR/EMC clock, raise camera QoS priority, or **reduce passes** (zero-copy,
fewer ISP stages, compression).

---

## 3. QoS and bandwidth voting

SoCs let clients **vote** for bus bandwidth and priority:

```
 Qualcomm: CPAS votes CAMNOC AXI bandwidth + AHB clock per use-case (Module 11).
           BW vote must cover all active IFE write-masters + reprocess.
 NVIDIA:   EMC (external memory controller) frequency + ISO/latency clients;
           camera is an ISO (isochronous) client needing guaranteed bandwidth.
 Generic:  interconnect framework (icc_set_bw), devfreq for DDR scaling.
```

```
 Symptom: IFE/CSID overflow only under load (other apps running)
 Cause:   under-voted bandwidth OR DDR clock scaled down by devfreq OR a higher-
          priority client (display) starving camera
 Fix:     correct BW vote for the resolution/fps; mark camera as latency-critical/ISO;
          pin DDR/EMC clock during capture
```

Under-voting is the #1 cause of "works at 30fps, drops at 60fps" or "drops only when the
GPU/display is busy."

---

## 4. Latency: sources and reductions

### 4.1 Where latency comes from (glass-to-glass)
```
 exposure time            (1/fps worst case)         ← physics, can't avoid fully
 + sensor readout         (~1 frame)
 + CSI transport          (small)
 + ISP pipeline depth     (1-3 frames, shadow latch) ← Module 4/10
 + buffer queue depth     (N buffers = N frames)     ← vb2 ping-pong
 + encoder/compositor     (1+ frames)
 + display refresh        (up to 1 vsync)
 ─────────────────────────────────────────
 Glass-to-glass often 3-6 frames (50-100 ms at 60fps)
```

### 4.2 Reductions
```
 - Reduce buffer count to the minimum that still prevents drops (fewer in-flight frames)
 - Shorter exposure (more gain) where light allows → less integration latency
 - Fewer ISP stages / lighter TNR for low-latency modes (AR/VR need <20 ms)
 - Zero-shutter-lag (ZSL): keep a ring of recent frames so "capture" returns instantly
 - Bypass the encoder/compositor for preview (direct-to-display)
 - Higher fps shrinks per-frame latency (each stage = fewer ms)
```

For **AR/VR/automotive** the latency budget is tight (tens of ms); for stills, throughput
and quality dominate over latency.

---

## 5. DMA and buffer optimization

```
 - Ping-pong with the MINIMUM buffers needed (Module 13): more buffers = more latency,
   fewer = risk of drops. Tune to just cover IRQ/scheduling jitter.
 - Use scatter-gather + SMMU (dma-sg) to avoid large contiguous (CMA) allocation stalls
   that cause REQBUFS latency/failures (Module 14).
 - Align strides/bytesperline to the hardware's burst size for efficient DDR bursts
   (misaligned stride = wasted bandwidth + partial bursts).
 - Avoid USERPTR (cache/IOMMU overhead); prefer MMAP/DMABUF.
```

---

## 6. Cache and coherency optimization

```
 - For frames the CPU never touches (camera→encoder zero-copy), skip CPU cache
   maintenance entirely (the data never enters CPU cache) → saves invalidate cost.
 - Use V4L2_BUF_FLAG_NO_CACHE_* / non-coherent hints when the CPU doesn't read the
   buffer, avoiding needless cache ops per frame (Module 13/14).
 - Conversely, if the CPU DOES process frames, ensure proper invalidation (correctness
   over speed) — a stale frame is worse than a slow one.
 - Cache maintenance on a 12 MB frame at 60fps is real bandwidth; eliminate it via
   zero-copy where possible.
```

---

## 7. ISP throughput

```
 ISP processes at a fixed pixels/clock; max throughput = isp_clock × pixels_per_clock.
 If input pixel rate > ISP throughput → overflow.

 Levers:
   - Raise ISP/IFE clock (costs power)
   - Reduce work: bin/crop at the sensor (Module 3) so fewer pixels reach the ISP
   - Split across multiple IFEs (dual-IFE for high-res, Module 11)
   - Offload snapshot processing to offline engines (BPS/IPE) so the real-time path
     stays within budget
```

```
 Dual/triple-IFE: very high-res sensors (e.g. 108MP, 8K) exceed one IFE's throughput,
 so the frame is split (left/right halves or stripes) across multiple IFEs and stitched.
```

---

## 8. Multi-camera scaling

```
 N cameras multiply CSI lanes, ISP passes, and DDR bandwidth.
 Concurrency constraints:
   - total CSI lanes / CSIPHY instances
   - number of IFEs vs number of simultaneous PIX streams (RDI is cheaper)
   - DDR bandwidth budget for N × frame_size × fps × passes
   - power/thermal ceiling

 Strategies:
   - Use RDI (raw) for secondary cameras, process on demand (saves IFE + BW)
   - Lower resolution/fps for non-primary streams
   - Time-share IFEs where simultaneous full processing isn't required
   - Frame-sync sensors (Module 3) so stitching/depth is correct and buffered uniformly
```

Surround-view (4-6 cameras) and stereo are where bandwidth/QoS budgeting becomes the
dominant design activity.

---

## 9. Power and thermal

```
 Camera is a major power/thermal load (sensor + CSI + ISP + DDR traffic + encoder).
 Thermal throttling reduces ISP/DDR/CPU clocks → frame drops appear "randomly" after
 minutes of recording.

 Optimizations:
   - Runtime PM: power-gate blocks when idle (Module 9), autosuspend between sessions
   - Right-size clocks: don't run ISP/DDR faster than the use-case needs
   - Prefer hardware blocks over CPU (encode on NVENC/Venus, not CPU)
   - Zero-copy to cut DDR traffic (DDR is a big power consumer)
   - Drop to lower fps/resolution under thermal pressure gracefully (HAL policy)
```

```
 "Records fine for 3 minutes then drops frames" → thermal throttling, not a logic bug.
 Confirm with thermal zone temps + clock summary over time.
```

---

## 10. Measurement methodology

```bash
# fps and drops
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=600   # reports fps; watch for drops

# frame interval jitter via ftrace vb2 timestamps (Module 15)
cd /sys/kernel/debug/tracing; echo 1 > events/vb2/enable; cat trace_pipe

# DDR/interconnect bandwidth + clocks over time
cat /sys/kernel/debug/clk/clk_summary | grep -iE "cam|emc|ddr|camnoc"
cat /sys/class/devfreq/*/cur_freq          # DDR/EMC devfreq scaling
# interconnect (where exposed):
cat /sys/kernel/debug/interconnect/interconnect_summary

# overflow / drop counters
dmesg | grep -iE "overflow|drop|frame.*lost|sof.*missed"

# thermal
cat /sys/class/thermal/thermal_zone*/temp
```

```
 Methodology:
   1. Establish the target (e.g. 4K30 sustained, <X ms latency).
   2. Compute the bandwidth budget (Σ passes × frame_size × fps + other clients).
   3. Measure actual fps/drops and overflow counters under realistic load.
   4. Correlate drops with bandwidth votes / DDR clock / thermal over time.
   5. Apply the cheapest lever first (QoS vote, then clocks, then reduce passes/res).
```

---

## 11. Interview Q&A

**Q1. Camera works at 1080p30 but drops frames at 1080p60. Where do you look first?**
Bandwidth/QoS, not the sensor. Doubling fps doubles CSI and DDR bandwidth. Check the
bandwidth vote (CPAS/CAMNOC or EMC/interconnect) covers the higher rate, that the DDR/EMC
clock isn't scaled down, and CSID/IFE overflow counters. It's almost always under-voted
bandwidth or a capped DDR clock.

**Q2. Estimate the DDR bandwidth for 4K30 NV12 with temporal NR and encoding.**
Frame ≈ 3840×2160×1.5 ≈ 12 MB. Passes: IFE write, TNR reference read, ISP write, encoder
read, display read ≈ 5 passes. 12 MB × 30 × 5 ≈ 1.8 GB/s for camera alone, plus all other
DDR clients. The point: multiply frame size × fps × number of read/write passes, then add
the rest of the system.

**Q3. What causes IFE/CSID overflow interrupts?**
The write-master can't drain pixels to DDR fast enough — DDR QoS starvation from
under-voted bandwidth, a down-scaled DDR clock, or higher-priority clients (display)
hogging the bus. The input keeps arriving, the output FIFO fills, and it overflows. It's a
bandwidth/scheduling problem (Modules 8/11), not a PHY problem (which would be CRC).

**Q4. Where does glass-to-glass latency come from and how do you reduce it?**
Exposure + sensor readout + CSI + ISP pipeline depth (shadow latch, 1-3 frames) + buffer
queue depth (N frames) + encoder/compositor + display vsync — typically 3-6 frames.
Reduce by: minimizing buffer count, shorter exposure, lighter/fewer ISP stages for
low-latency modes, ZSL, and bypassing the encoder/compositor for preview.

**Q5. How does zero-copy improve performance beyond saving CPU cycles?**
It removes entire DDR read+write passes (no memcpy of 12 MB frames) and the associated
cache maintenance, cutting DDR bandwidth and power — often the actual bottleneck. dma-buf
sharing (Module 14) lets camera→encoder→display use one buffer, freeing bandwidth for
higher resolution/fps.

**Q6. A device records fine for a few minutes then starts dropping frames. Diagnosis?**
Thermal throttling: sustained camera+encoder+DDR load heats the SoC, the thermal governor
lowers ISP/DDR/CPU clocks, and the pipeline can no longer meet bandwidth → drops. Confirm
by logging thermal zone temps and clock frequencies over time; it correlates with
temperature, not with any code path.

**Q7. How do you scale to a 6-camera surround-view system?**
Budget total CSI lanes, IFE instances, and DDR bandwidth (6 × frame_size × fps × passes).
Use RDI/raw for secondary cameras and process on demand, lower resolution/fps for
non-primary streams, time-share IFEs, frame-sync the sensors, and ensure the QoS/bandwidth
vote covers the aggregate. The bottleneck is DDR bandwidth and IFE count, not any single
sensor.

**Q8. Input pixel rate exceeds one ISP's throughput (e.g. 108MP). Options?**
Reduce work before the ISP (sensor binning/crop), raise the ISP clock (power cost), or
split the frame across multiple IFEs (dual/triple-IFE stripes stitched together). For
snapshots, offload to offline engines (BPS/IPE) so the real-time preview path stays within
throughput while full-quality processing happens asynchronously.

---

### Key takeaways
- Most camera performance problems are bandwidth/scheduling: budget DDR as Σ(read+write
  passes) × frame_size × fps + all other clients; under-voted QoS or capped DDR clock →
  overflow → drops.
- Latency is the sum of exposure + readout + ISP depth + buffer count + encoder/display;
  reduce buffers, ISP stages, and use ZSL/zero-copy for tight budgets.
- Zero-copy (dma-buf) removes whole DDR passes and cache work — often the biggest win.
- "Drops at higher fps" = bandwidth/QoS; "drops after minutes" = thermal throttling;
  multi-camera and high-res push you to RDI offload and multi-IFE splitting.
