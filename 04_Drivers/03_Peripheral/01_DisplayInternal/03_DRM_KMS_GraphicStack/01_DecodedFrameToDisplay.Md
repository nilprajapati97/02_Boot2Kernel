Here’s **Step 2** rewritten, tightened up, and expanded “from scratch” with the **kernel/DRM KMS point of view**—plus a compact block diagram you can paste at the end of your post.

---

# 🎥 Step 2 — From Decoded Frames to the Display: DRM/KMS Deep-Dive

## 0) Mental model (one minute version)

* **Userspace** gives the kernel a **buffer** that already holds pixels (YUV/RGB).
* **DRM/KMS** programs the display pipeline so hardware can **scan out** that buffer to a panel/connector at the right **mode timing** (resolution, refresh).
* The kernel does this by wiring up 5 object types: **Framebuffer → Plane → CRTC → Encoder → Connector**.
  Atomic properties glue it all together.

---

## 1) The KMS objects (what they map to in real hardware)

### 🧱 Framebuffer (FB)

* A **descriptor** that tells KMS where pixels live in memory and how to read them:

  * BO handle or **DMA-BUF FD**, width/height, **fourcc** (e.g., `DRM_FORMAT_NV12`, `DRM_FORMAT_XRGB8888`), pitches, offsets, **modifier** (tiling/compression like AFBC).
* FB is **not** a copy; it just **describes** memory the display engine will read.

### 🪟 Plane

* A **layer** the display controller can place/scale on screen.
* Types:

  * **Primary** (full-screen base layer),
  * **Overlay** (video layer, often supports YUV + scaling/CSC),
  * **Cursor**.
* Per-plane properties: `FB_ID`, `CRTC_ID`, `SRC_X/Y/W/H`, `CRTC_X/Y/W/H`, `rotation`, `zpos`, `COLOR_ENCODING`/`COLOR_RANGE`, etc.

### ⏱️ CRTC

* The **scanout engine** (timing generator). Reads pixels from its attached planes, applies composition/scaling/color-management, and produces a pixel stream.
* Owns the **vblank** interrupt (used for tear-free flips).

### ⚙️ Encoder

* Converts the CRTC’s pixel stream into link-specific signaling (HDMI, **MIPI-DSI**, eDP, LVDS).
* In SoCs, encoders are often implemented with **DRM bridge** drivers chained to the CRTC.

### 🔌 Connector

* The **physical output** (HDMI receptacle, DSI panel).
* Exposes **modes** (timings). DSI panels typically use a **panel driver** that provides the mode and init sequences (DCS commands).

---

## 2) Memory & buffers (how frames get into KMS)

### Where the pixels come from

* **CPU/GPU** render → GEM/GBM BO → FB.
* **Hardware video decoder (V4L2 M2M)** → exported as **DMA-BUF** → imported by KMS → FB.
  *This is the zero-copy path for smooth video.*

### Formats & modifiers

* Common fourcc: `NV12` (multi-planar YUV), `YUYV` (packed), `XRGB8888` (RGB).
* **Modifiers** describe layout/compression (linear, tiled, AFBC, UBWC…). Atomic `addfb2` takes them; drivers **reject** unsupported combos in `atomic_check`.

### CMA / IOMMU / cache

* Many display drivers allocate with **GEM CMA** (phys-contiguous) or accept **DMA-BUF** backed by an **IOMMU**.
* With DMA-BUF, **sync fences** ensure cache coherency and producer/consumer ordering (see §5).

---

## 3) Atomic modesetting (how the kernel commits a frame)

**Userspace (libdrm)** builds an **atomic state**:

1. Pick a **connector** + **mode**, attach to a **CRTC**.
2. Choose a **plane** and set properties: `FB_ID`, source/destination rectangles, colorspace.
3. Optionally request **non-blocking** commit with an **out-fence** for completion.

**Kernel driver path**:

* `atomic_check()`
  Validates bandwidth, scaler taps, format support, plane overlap, link clocking, etc.
* `atomic_commit()` (often split into **software** + **hardware** phases)

  * Program PLL/PHY, encoders, CRTC timings (if modeset).
  * Program plane registers with FB addresses, pitches, format.
  * Arm **page-flip** to occur on next **vblank** (tear-free).
* **vblank IRQ** fires → driver signals **page-flip event** back to userspace.

Result: the **next refresh** scans out the **new FB**.

---

## 4) Composition paths (how many planes you really need)

* **Best-case video**: put the decoded **YUV** buffer directly on a **video overlay plane** that supports YUV + scaling + CSC → **no GPU blit**.
* If hardware lacks YUV scanout/CSC/scaler:

  * Use GPU/VPU to convert to RGB or pre-scale → present via primary plane.
* Plane stacking via `zpos` lets you place **UI (RGB)** on primary and **video (YUV)** on overlay simultaneously.

---

## 5) Synchronization: fences & vblank (why no tearing)

* **Implicit sync** (classic PRIME): importing a DMA-BUF carries an internal **dma-fence**; KMS waits until the producer (decoder) signals completion.
* **Explicit sync**: userspace passes `IN_FENCE_FD`/receives `OUT_FENCE_PTR` atomic properties for tighter control.
* **vblank** is the safe window to **flip**; KMS schedules flips to **vblank** by default (tear-free).

---

## 6) Colorspace & quality knobs

* YUV → RGB **CSC** (BT.601/BT.709/BT.2020) and **range** (full/limited) are plane properties on many drivers.
* CRTC often exposes **degamma/CSCs/gamma LUTs** and **CTM** matrices for color management.
* If your panel expects limited range or a specific matrix, set the properties at commit time.

---

## 7) MIPI-DSI specifics (how KMS reaches a DSI panel)

* The KMS **CRTC** feeds a **DSI encoder**; a **panel driver** (via `drm_panel`) provides mode timings and runs panel **DCS** init (`prepare/enable/disable`).
* During a modeset:

  1. Program timings in CRTC.
  2. Bring up DSI PHY/PLL.
  3. Send panel **init sequences** (tear-on, pixel format, lanes, porch, backlight).
* After commit, pixel stream is packetized to **DSI high-speed** lanes.

---

## 8) Minimal userspace flows

### A) Pure KMS (libdrm) “single-plane scanout” (conceptual)

```c
// open DRM, pick connector+mode, get crtc
drmModeAtomicReq *req = drmModeAtomicAlloc();
add_plane_props(req, plane_id, FB_ID=fb, CRTC_ID=crtc,
                SRC_W/H=width<<16, CRTC_W/H=width,height);
add_crtc_props(req, crtc, MODE_ID=blob_for(mode), ACTIVE=1);
drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, user);
```

### B) Zero-copy with GStreamer to KMS

```bash
# Hardware decode to NV12 → direct KMS scanout
gst-launch-1.0 filesrc location=video.h264 ! h264parse ! v4l2h264dec ! \
  video/x-raw,format=NV12 ! kmssink connector-id=<id> plane-id=<video_plane>
```

* `v4l2h264dec` exports **DMA-BUFs**; `kmssink` imports them and performs **atomic commits**.
* Add `render-rectangle=x,y,w,h` to place/scale on the plane.

---

## 9) Debugging checklists

* **Topology**: `modetest -c -p -M <driver>` shows CRTCs/planes/encoders/connectors.
* **Driver state**: `/sys/kernel/debug/dri/0/state` (read-only dump of current atomic state).
* **vblank/pageflips**: enable DRM debug logs; ensure no **underflow** messages (bandwidth).
* **Format support**: try `modetest --v` to probe plane formats/modifiers.

---

## 10) “What actually happens on a page flip?”

1. Userspace builds **atomic state** (new FB + plane placement) and submits.
2. Driver’s `atomic_check()` validates clocks/bandwidth/format.
3. Driver latches new registers; schedules flip at **next vblank**.
4. On vblank IRQ, hardware switches base addresses; driver raises **page-flip event**.
5. Userspace recycles the old buffer; decoder starts filling the next one.

---

## 11) End-to-end block flow (decoded frame → panel)

```
Decoded Frame (DMA-BUF: NV12/RGB)
           │
           ▼
  DRM Framebuffer (FB)
  (width/height/fourcc/modifier/pitches)
           │
           ▼
        Plane
   (crop/scale/CSC/zpos)
           │
           ▼
         CRTC
   (timings, vblank, color mgmt)
           │
           ▼
        Encoder
   (HDMI / MIPI-DSI / LVDS)
           │
           ▼
       Connector
   (port/panel, mode)
           │
           ▼
      Display Panel
   (scanned out at refresh rate)
```

---

### TL;DR for your slide

* **Give KMS a buffer**, not pixels.
* **Atomic commit** wires **FB→Plane→CRTC→Encoder→Connector**.
* **Vblank** flips → **tear-free**.
* Use **DMA-BUF** to stay **zero-copy** from decoder to display.

If you want, I can add a **one-screen “modetest + kmssink quick start”** tailored to your SoC (you tell me the DRM driver and panel), so readers can reproduce this on real hardware.
