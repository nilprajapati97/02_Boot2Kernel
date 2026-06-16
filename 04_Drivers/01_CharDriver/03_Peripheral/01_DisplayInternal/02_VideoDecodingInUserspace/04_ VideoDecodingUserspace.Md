Step 1: Userspace Video Decoding in the Linux Display Pipeline
==============================================================================
Awesome topic—this is the part where compressed bits become actual pixels your display stack can use. I’ll walk from **userspace → kernel → back to userspace → display**, covering containers, parsers, decoders (CPU & HW), buffer formats, synchronization, and how decoded frames reach DRM/KMS with zero-copy.

---

# 0) What “decoding” actually means

* **Container** (MP4/MKV/TS) = a box holding **streams** (video, audio, subtitles) with **timestamps** (PTS/DTS) and metadata.
* **Codec** (H.264/HEVC/VP9/AV1) = compression algorithm used *inside* the video stream.
* **Decoding** = turning compressed **bitstream** → **raw image frames** (usually **YUV**, sometimes **RGB**) that the graphics/display pipeline can scan out.

---

# 1) Demuxing (container parsing): where playback begins

**Userspace (VLC/GStreamer/FFmpeg)**:

* **Demuxer** reads the file/stream, splits it into separate tracks and produces **timestamped packets** for each track.
* Extracts codec **extradata** (SPS/PPS/VPS for H.264/HEVC, codec private data) and attaches it to the first packets.
* Maintains **A/V sync** by keeping PTS/DTS and a **playback clock** (GStreamer’s global clock or FFmpeg’s `av_sync_type`).
* Handles **seeking** by using the container’s index (MOOV, CUE, etc.).

**Display implications**

* Correct **timestamps** are critical. If PTS/DTS are wrong, display will stutter or desync from audio.
* **Variable frame rate** material depends on timestamps, not a fixed fps.

**GStreamer mental model**

```
filesrc → <container demux> → h264parse → decoder → video/x-raw → sink
```

---

# 2) Bitstream parsing (aligning for the decoder)

**Why parse?**

* Decoders expect a specific **bitstream format**:

  * H.264: **Annex-B** (start codes) vs **AVCC** (length-prefixed).
* Parsers (e.g., `h264parse`, `h265parse`) ensure:

  * Consistent alignment (one frame per buffer, or necessary slices).
  * Proper **codec headers** (SPS/PPS/VPS) are present when needed.
  * Correct **caps**: width/height/profile/level/format.

**Kernel tie-in**

* Hardware decoders (especially **stateless** V4L2) often require clean **per-frame metadata** (reference lists, quant matrices). Parsers (and the decoder element) assemble that.

---

# 3) Choosing the decoder: software vs hardware

### A) Software decoding (CPU; libavcodec, GStreamer software decoders)

* Pros: works everywhere, robust features, no vendor constraints.
* Cons: **CPU heavy**, power hungry on embedded boards.
* Outputs: system memory frames (e.g., **NV12**, **I420**, **Y444**, **RGB**).
* Often needs a **colorspace convert** before display (display planes typically prefer NV12/XRGB).

**Examples**

* GStreamer (CPU):

  ```bash
  gst-launch-1.0 filesrc location=vid.mp4 ! qtdemux ! h264parse ! avdec_h264 ! videoconvert ! autovideosink
  ```
* FFmpeg (CPU):

  ```bash
  ffplay -vcodec h264 -i vid.mp4
  ```

### B) Hardware decoding (dedicated VPU)

* Pros: **Low power, high throughput**, keeps CPU free.
* Cons: Platform-specific APIs, formats, and constraints.

**Common APIs**

* **V4L2 M2M (Linux kernel)** – generic across many ARM SoCs (Rockchip/Hantro/Exynos/MTK/Allwinner/etc.).
* **VA-API** – Intel iGPU (and some AMD).
* **NVDEC** – NVIDIA (desktop dGPU and some embedded; Jetson uses V4L2/NvMM variants).

---

# 4) V4L2 M2M decoding (kernel perspective, then userspace)

**Concept**: A single `/dev/videoX` node with two queues:

* **OUTPUT (compressed in)**: app enqueues H.264/HEVC packets.
* **CAPTURE (raw out)**: driver returns decoded frames.

Two decoder families:

1. **Stateful**: driver manages reference frames/DPB internally. App feeds compressed stream + config; driver figures it out.
2. **Stateless + Request API**: app (or media framework) provides **per-frame parameters** (slice params, ref lists). GStreamer/FFmpeg handle this under the hood via the decoder elements.

**Kernel flow (simplified)**

```
open /dev/videoX
VIDIOC_S_FMT on OUTPUT (e.g., V4L2_PIX_FMT_H264)
VIDIOC_S_FMT on CAPTURE (e.g., V4L2_PIX_FMT_NV12M)
REQBUFS & QBUF on both queues
STREAMON both queues
DQBUF CAPTURE to get decoded frames
```

**Zero-copy to display**

* Many V4L2 decoders can **export DMABUF** for CAPTURE buffers.
* Those DMABUF FDs can be **imported by DRM/KMS** (kms sink) or by GPU (GL/Vulkan) → avoids CPU copies.

**GStreamer examples (HW decode to KMS)**

```bash
# Generic V4L2 decoder → KMS (scanout) with zero-copy on capable SoCs
gst-launch-1.0 filesrc location=vid.h264 ! h264parse ! v4l2h264dec ! \
  video/x-raw,format=NV12 ! kmssink render-rectangle="0,0,1280,720"
```

**Common V4L2 colorspaces**

* **NV12** (Y plane + interleaved UV 4:2:0) is the de-facto decode output.
* 10-bit content often outputs **P010** (16-bit container, 10-bit data).

---

# 5) VA-API decoding (Intel path)

**Idea**: The iGPU’s media engine outputs decoded frames into **VA surfaces** (GPU-accessible). You can:

* Display directly with **vaapisink** (zero-copy path), or
* Map/copy to system memory if needed (slower).

**Example (GStreamer)**

```bash
gst-launch-1.0 filesrc location=vid.h264 ! qtdemux ! h264parse ! vaapih264dec ! \
  videoconvert ! autovideosink
```

**Better zero-copy**:

```bash
gst-launch-1.0 filesrc location=vid.h264 ! qtdemux ! h264parse ! vaapih264dec ! vaapisink
```

---

# 6) NVDEC decoding (NVIDIA)

On desktops:

* FFmpeg uses `-hwaccel cuda` / `-c:v h264_cuvid`(legacy) or `-c:v h264_nvdec`.
* GStreamer uses the **nvcodec** plugins (`nvh264dec`, `nvh265dec`), and for display **glimagesink**/**nvoverlaysink**.

Example:

```bash
gst-launch-1.0 filesrc location=vid.h265 ! h265parse ! nvh265dec ! \
  video/x-raw(memory:GLMemory),format=NV12 ! glimagesink
```

On **Jetson**, the stack is different (V4L2/NvMM, e.g., `nvv4l2decoder` + `nveglglessink`) but conceptually the same.

---

# 7) Frame formats, color science & post-processing

* Most decoders output **YUV 4:2:0** (NV12/I420). Displays often prefer **NV12** or **XRGB8888** for scanout.
* If your plane can’t handle YUV, you need **colorspace conversion** (CSC) to RGB using:

  * GPU shaders (`glimagesink`, `kmssink + GPU layer`),
  * Display controller’s **overlay scaler/CSC** (some DRM planes do YUV→RGB in hardware),
  * CPU (slow).
* **HDR** (PQ/HLG, BT.2020, mastering metadata): handoff to display requires passing **colorspace/range/metadata**. In DRM/KMS this may involve:

  * Plane/CRTC **COLOR\_ENCODING / COLOR\_RANGE** properties,
  * HDR metadata blobs (if driver/panel support it).

---

# 8) Synchronization, latency, and reordering

* **Frame reordering** (B-frames) means the decoder outputs frames **not necessarily in input order**. Timestamps (PTS) keep them in presentation order.
* **Playback clock** (GStreamer clock) regulates when a decoded frame is **presented**.
* **Zero-copy pipelines** must honor sync using **fences** (DMA-BUF implicit or explicit). KMS flips on **vblank** to avoid tearing.
* **Latency levers**: parser alignment, decoder queue depth, sink’s max-lateness, and plane scaling.

---

# 9) Putting it all together with display

### A) CPU decode → generic desktop sink

```bash
gst-launch-1.0 filesrc location=vid.mp4 ! qtdemux ! h264parse ! avdec_h264 ! \
  videoconvert ! autovideosink
```

### B) HW decode (V4L2) → DRM/KMS direct (embedded board)

```bash
# Zero-copy on capable SoCs: decoded NV12 DMABUF → KMS plane
gst-launch-1.0 filesrc location=vid.h264 ! h264parse ! v4l2h264dec ! \
  video/x-raw,format=NV12 ! kmssink plane-id=<overlay_plane> connector-id=<conn>
```

### C) Intel VA-API → zero-copy display

```bash
gst-launch-1.0 filesrc location=vid.h265 ! qtdemux ! h265parse ! vaapih265dec ! vaapisink
```

### D) NVIDIA NVDEC → GL display

```bash
gst-launch-1.0 filesrc location=vid.h264 ! qtdemux ! h264parse ! nvh264dec ! \
  video/x-raw(memory:GLMemory) ! glimagesink
```

---

# 10) Debugging & capability discovery

**Userspace**

* `gst-inspect-1.0 | grep -E '(h264|vaapi|v4l2|nvdec)'` – list decoders.
* `GST_DEBUG=*:3,v4l2:6,vaapi:6 kmssink:6` – turn up logs.
* `ffmpeg -hwaccels` / `ffmpeg -decoders | grep nvdec`.

**Kernel/V4L2**

* `v4l2-ctl --list-devices` / `--list-formats-out` / `--list-formats-capture`.
* `dmesg` for decoder/DRM warnings (bandwidth, unsupported formats).
* Ensure **DMABUF import/export** is supported (required for zero-copy).

**Display**

* `modetest -c -p` – list connectors/planes/formats.
* Check that your plane supports **NV12** and scaling if you want overlay scanout.

---

# 11) Quick glossary (handy during reviews)

* **Demuxer**: extracts elementary streams from a container.
* **Parser**: normalizes codec bitstream for the decoder.
* **Decoder**: turns compressed video into raw frames.
* **V4L2 M2M**: kernel API for offloading decode/encode to hardware.
* **DMABUF**: file-descriptor handle to share GPU/VPU buffers zero-copy.
* **DRM/KMS**: kernel display stack that scans out buffers to panels.
* **Plane/CRTC**: display layers and scanout engine in KMS.
* **NV12/I420/P010**: common YUV pixel formats from decoders.

---

## TL;DR

1. **Demux** the container → timestamped packets.
2. **Parse** bitstream → decoder-friendly alignment + headers.
3. **Decode** via CPU (portable) or HW (V4L2/VAAPI/NVDEC) → YUV frames.
4. **Zero-copy** those frames to display using **DMABUF** → **DRM/KMS** plane → vblank-synchronized scanout.
5. If needed, do **CSC/scaling** either in GPU or display hardware.

If you tell me your SoC/board, I can give an **exact GStreamer pipeline** (decoder element + `kmssink` arguments) that hits zero-copy on your hardware.
