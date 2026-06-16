# 14_Camera_Subsystems

## Scope
A complete, depth-first learning series on the **Linux Camera Subsystem**, written for
senior Linux kernel / BSP / device-driver engineers. The series goes end to end —
from photons and CMOS pixels, through MIPI CSI-2 and the V4L2/Media-Controller
frameworks, into real sensor and ISP drivers, the Qualcomm and NVIDIA SoC camera
architectures, memory/DMA/SMMU, debugging, performance, the Android camera stack, and
a consolidated interview Q&A bank.

Each module follows the same structure: **Why / What / When / Where / How**, with ASCII
hardware/timing/sequence diagrams, Linux kernel source walkthroughs (structs, callbacks,
APIs, file paths), ARM64 + SoC specifics (Qualcomm, NVIDIA), memory/DMA flow, real
boot-time and streaming call flows, debugging techniques, common bugs, and interview
Q&A at the end.

## Reading order (each module builds on the previous)
```
 01 Camera Basics → 02 Hardware → 03 CMOS Sensor → 04 Image Processing
   → 05 Media Framework → 06 V4L2 → 07 Device Tree → 08 MIPI CSI-2
   → 09 Sensor Driver → 10 ISP Driver → 11 Qualcomm → 12 NVIDIA
   → 13 videobuf2 → 14 Memory/DMA → 15 Debugging → 16 Performance
   → 17 Android Camera Stack → 18 Interview Questions
```

## Subdirectories
- None

## Files
- [01_Introduction_to_Digital_Cameras.md](./01_Introduction_to_Digital_Cameras.md) — optics, exposure/gain, shutter, ISO, HDR, noise; mapped to V4L2 controls.
- [02_Camera_Hardware_Architecture.md](./02_Camera_Hardware_Architecture.md) — full block diagram, power/clock/reset/I2C, SerDes, schematic-to-driver bring-up.
- [03_CMOS_Image_Sensor.md](./03_CMOS_Image_Sensor.md) — pixel/photodiode, Bayer, ADC, HTS/VTS timing, PLL, binning, RAW formats.
- [04_Image_Processing_Fundamentals.md](./04_Image_Processing_Fundamentals.md) — the ISP pipeline (BLC→LSC→demosaic→CCM→gamma→NR→CSC), color spaces, 3A.
- [05_Linux_Media_Framework.md](./05_Linux_Media_Framework.md) — Media Controller: entities/pads/links, subdevs vs video nodes, pipeline validation, media-ctl.
- [06_V4L2_Framework.md](./06_V4L2_Framework.md) — V4L2 ioctls, formats, controls, buffer models, streaming state machine, Request API, async/fwnode.
- [07_Device_Tree_for_Cameras.md](./07_Device_Tree_for_Cameras.md) — OF graph ports/endpoints, CSI lanes/link-freq, clocks/regulators/GPIO, Qualcomm + Tegra DT.
- [08_MIPI_CSI2_Protocol.md](./08_MIPI_CSI2_Protocol.md) — D-PHY/C-PHY, packets, virtual channels, data types, ECC/CRC, FS/FE sync, settle timing.
- [09_Camera_Sensor_Driver.md](./09_Camera_Sensor_Driver.md) — full I2C sensor driver: probe, power sequence, regmap, controls, s_stream, suspend/resume.
- [10_Camera_ISP_Driver.md](./10_Camera_ISP_Driver.md) — ISP driver patterns, block config, stats/params DMA, IRQ/frame-done, rkisp1 walkthrough.
- [11_Qualcomm_Camera_Architecture.md](./11_Qualcomm_Camera_Architecture.md) — CAMSS/Spectra: CSIPHY/CSID/IFE/ICP/BPS/IPE, RDI vs PIX, CPAS, CamX/CHI, Camera Request Manager.
- [12_NVIDIA_Camera_Architecture.md](./12_NVIDIA_Camera_Architecture.md) — Tegra/Jetson: NVCSI/VI/ISP, host1x + syncpoints, tegra-video, libargus, tegracam DT modes.
- [13_videobuf2_Framework.md](./13_videobuf2_Framework.md) — vb2_queue, buffer lifecycle, mem backends (contig/sg/vmalloc), cache coherency, ping-pong.
- [14_Camera_Memory_Management.md](./14_Camera_Memory_Management.md) — DMA API, IOMMU/SMMU, cache coherency, CMA/reserved-memory, dma-buf, dma-heap, SMMU faults.
- [15_Camera_Debugging.md](./15_Camera_Debugging.md) — media-ctl/v4l2-ctl/yavta, dynamic debug, debugfs, ftrace, layer isolation, scope/MIPI analyzer.
- [16_Camera_Performance_Optimization.md](./16_Camera_Performance_Optimization.md) — bandwidth/DDR/QoS budgeting, latency, DMA/cache, ISP throughput, multi-camera, thermal.
- [17_Android_Camera_Stack.md](./17_Android_Camera_Stack.md) — CameraX/Camera2 → CameraService → HAL3 → vendor HAL → kernel; request/result, gralloc/dma-heap, fences.
- [18_Linux_Camera_Interview_Questions.md](./18_Linux_Camera_Interview_Questions.md) — categorized master Q&A bank + real scenario and system-design questions.

## Cross-references
- Memory/DMA/IOMMU foundations: `../06_MemoryManagement/02_Memory_Topics/` (DMA, IOMMU,
  cache coherency, CMA) — Module 14 applies these to the camera datapath.
- Device drivers / device tree: `../04_Drivers/`, `../05_DeviceTree/`.

## Key kernel source anchors
- `drivers/media/v4l2-core/`, `drivers/media/common/videobuf2/`
- `include/media/v4l2-*`, `include/media/media-*`
- `include/uapi/linux/videodev2.h`, `media.h`, `v4l2-subdev.h`, `rkisp1-config.h`
- `drivers/media/i2c/` (imx219, imx290, ov5640 — sensor driver references)
- `drivers/media/platform/qcom/camss/` (Qualcomm CAMSS)
- `drivers/staging/media/tegra-video/` (NVIDIA Tegra VI/CSI)
- `drivers/media/platform/rockchip/rkisp1/` (open ISP reference)
- `Documentation/userspace-api/media/`, `Documentation/devicetree/bindings/media/`

## Notes
- Kernel baseline for source references: mainline ~6.6 LTS, with downstream vendor
  differences called out for Qualcomm (techpack/CamX) and NVIDIA (L4T/Argus).
- Diagrams are ASCII so they render in any viewer; code blocks are illustrative and
  follow mainline driver idioms.
