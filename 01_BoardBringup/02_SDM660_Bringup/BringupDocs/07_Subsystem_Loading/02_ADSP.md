# ADSP — Audio DSP

## Overview

The **ADSP (Audio DSP)** is a Hexagon 680 processor that handles audio processing, voice calls, and audio effects offloaded from the main CPU. It runs the Qualcomm **AudioReach/q6asm** firmware and communicates with the Apps processor via APR (Audio Processing Router) over GLINK/SMD.

---

## ADSP Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Apps Processor (Linux)                                   │
│  ├── ALSA/ASoC framework                                │
│  ├── q6asm / q6adm / q6afe kernel drivers              │
│  └── APR (Audio Processing Router) ──┐                  │
│                                       │ GLINK/SMD        │
│  ┌────────────────────────────────────▼───────────────┐  │
│  │  ADSP (Hexagon 680)                                │  │
│  │  ├── Audio Stream Manager (ASM)                    │  │
│  │  ├── Audio Device Manager (ADM)                    │  │
│  │  ├── Audio Front End (AFE)                         │  │
│  │  ├── Audio codecs (software decode/encode)         │  │
│  │  └── Voice processing                              │  │
│  └────────────────────────────────────────────────────┘  │
│                        │                                  │
│                   Audio Codec HW                          │
│                   (WCD9340/9335)                          │
└──────────────────────────────────────────────────────────┘
```

---

## ADSP Loading

```
PIL loads ADSP firmware:
1. request_firmware("adsp.mdt") from /lib/firmware/
2. Load segments to reserved DDR @ 0x92a00000
3. TZ authenticates and releases ADSP from reset
4. ADSP boots Hexagon RTOS (QuRT)
5. ADSP signals ready via SMSM
6. Kernel APR driver connects to ADSP services

Kernel log:
[    3.200] subsys-pil-tz 15400000.qcom,lpass: firmware: requesting adsp.mdt
[    3.500] subsys-pil-tz 15400000.qcom,lpass: adsp: Brought up successfully
```

---

## Device Tree

```dts
adsp_pil: qcom,msm-adsp-loader {
    compatible = "qcom,adsp-pil-tz";
    qcom,firmware-name = "adsp";
    memory-region = <&adsp_fw_mem>;

    /* ADSP clock and power */
    clocks = <&rpmcc RPM_SMD_XO_CLK_SRC>;

    /* Subsystem restart */
    qcom,ssctl-instance-id = <0x14>;
};
```

---

## Audio Path (Kernel ↔ ADSP)

```
Audio Playback:
Android app → AudioTrack → AudioFlinger → HAL → ALSA →
  q6asm (kernel) → APR message → GLINK → ADSP ASM →
  ADSP AFE → Codec HW → Speaker/Headphone

Audio Capture:
Microphone → Codec HW → ADSP AFE → ADSP ASM →
  GLINK → APR → q6asm (kernel) → ALSA →
  HAL → AudioFlinger → Android app
```

---

## Related Documents

- [01_PIL_Framework.md](01_PIL_Framework.md) — Firmware loading
- [../08_IPC_Mechanisms/04_GLINK_SMD.md](../08_IPC_Mechanisms/04_GLINK_SMD.md) — ADSP communication transport
- [../08_IPC_Mechanisms/05_QMI.md](../08_IPC_Mechanisms/05_QMI.md) — QMI services on ADSP
