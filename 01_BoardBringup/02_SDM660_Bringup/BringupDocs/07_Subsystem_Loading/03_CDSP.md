# CDSP — Compute DSP

## Overview

The **CDSP (Compute DSP)** is a Hexagon 680 processor dedicated to compute-intensive tasks: camera processing (image stabilization, HDR, noise reduction), machine learning inference (SNPE/Qualcomm Neural Processing SDK), and general-purpose compute offload.

---

## CDSP Use Cases

| Use Case | Framework | API |
|----------|-----------|-----|
| Camera ISP processing | Qualcomm Camera2 | Camera HAL |
| Neural network inference | SNPE / QNN | Hexagon NN |
| Computer vision | FastCV | FastCV SDK |
| Custom compute | Hexagon SDK | IDL/FastRPC |

---

## CDSP Loading

```
PIL loads CDSP firmware:
1. request_firmware("cdsp.mdt") from /lib/firmware/
2. Load segments to reserved DDR @ 0x94800000 (6 MB)
3. TZ authenticates and releases CDSP from reset
4. CDSP boots QuRT RTOS
5. FastRPC framework connects Apps ↔ CDSP

Kernel log:
[    3.800] subsys-pil-tz 1a800000.qcom,cdsp: firmware: requesting cdsp.mdt
[    4.100] subsys-pil-tz 1a800000.qcom,cdsp: cdsp: Brought up successfully
```

---

## FastRPC (Apps ↔ CDSP Communication)

```
Apps Processor                           CDSP (Hexagon)
─────────────                            ──────────────
User-space app                           Hexagon shared object
    │                                        │
    ├── stub function call                   │
    │   (auto-generated from IDL)            │
    │                                        │
    ▼                                        │
/dev/adsprpc-cdsp                           │
    │                                        │
    ├── FastRPC driver (kernel)              │
    │   ├── Map shared buffers (ION/DMA)     │
    │   ├── Send RPC message via GLINK       │
    │   └── Wait for response                │
    │                                        ▼
    │                              skel function (CDSP side)
    │                              processes request
    │                              writes result to shared buffer
    │                                        │
    ◄────────── response ───────────────────┘
```

---

## Device Tree

```dts
cdsp_pil: qcom,msm-cdsp-loader {
    compatible = "qcom,cdsp-pil-tz";
    qcom,firmware-name = "cdsp";
    memory-region = <&cdsp_fw_mem>;
    qcom,ssctl-instance-id = <0x17>;
};
```

---

## Related Documents

- [01_PIL_Framework.md](01_PIL_Framework.md) — Firmware loading
- [04_SLPI.md](04_SLPI.md) — Sensor DSP (similar Hexagon)
- [../08_IPC_Mechanisms/04_GLINK_SMD.md](../08_IPC_Mechanisms/04_GLINK_SMD.md) — Transport layer
