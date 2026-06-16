# Qualcomm Snapdragon 660 (SDM660) — SoC Architecture Overview

## Introduction

The **Qualcomm Snapdragon 660 (SDM660)** is a premium-tier mobile SoC fabricated on a **14nm FinFET** process. It integrates heterogeneous compute (CPU, GPU, DSP), advanced connectivity, and multimedia capabilities into a single die with an external PMIC for power management.

This document provides a high-level architectural overview. Detailed deep-dives are in the [BringupDocs/00_SDM660_Architecture/](BringupDocs/00_SDM660_Architecture/) directory.

---

## SoC Block Diagram (Logical)

```
┌─────────────────────────────────────────────────────────────────┐
│                        SDM660 SoC                               │
│                                                                 │
│  ┌──────────────────────┐    ┌──────────────────────┐          │
│  │   CPU Subsystem       │    │   GPU Subsystem       │          │
│  │  ┌────────────────┐  │    │                        │          │
│  │  │ Gold Cluster    │  │    │   Adreno 509           │          │
│  │  │ 4x Kryo 260    │  │    │   Up to 650 MHz        │          │
│  │  │ @ 2.2 GHz      │  │    │                        │          │
│  │  │ L2: 1MB        │  │    └──────────────────────┘          │
│  │  └────────────────┘  │                                       │
│  │  ┌────────────────┐  │    ┌──────────────────────┐          │
│  │  │ Silver Cluster  │  │    │   DSP Subsystems       │          │
│  │  │ 4x Kryo 260    │  │    │                        │          │
│  │  │ @ 1.8 GHz      │  │    │  ┌─────┐ ┌─────┐     │          │
│  │  │ L2: 1MB        │  │    │  │ADSP │ │CDSP │     │          │
│  │  └────────────────┘  │    │  └─────┘ └─────┘     │          │
│  └──────────────────────┘    │  ┌─────┐              │          │
│                               │  │SLPI │              │          │
│  ┌──────────────────────┐    │  └─────┘              │          │
│  │   Memory Controller   │    └──────────────────────┘          │
│  │   BIMC (DDR4)         │                                       │
│  │   Dual Channel        │    ┌──────────────────────┐          │
│  └──────────────────────┘    │   Modem (MDM)          │          │
│                               │   X12 LTE              │          │
│  ┌──────────────────────────────────────────────────┐          │
│  │          Network-on-Chip (NoC) Interconnect        │          │
│  │   SNOC │ CNOC │ A2NOC │ BIMC │ MNOC │ GNOC        │          │
│  └──────────────────────────────────────────────────┘          │
│                                                                 │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌──────┐        │
│  │  I2C   │ │  SPI   │ │  UART  │ │  USB   │ │ UFS  │        │
│  │ (QUP)  │ │ (QUP)  │ │(BLSP)  │ │(DWC3)  │ │/eMMC │        │
│  └────────┘ └────────┘ └────────┘ └────────┘ └──────┘        │
│                                                                 │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────────────┐         │
│  │  DSI   │ │Camera  │ │ Video  │ │   RPM            │         │
│  │Display │ │  ISP   │ │ Venus  │ │ (Power Mgr)      │         │
│  └────────┘ └────────┘ └────────┘ └────────────────┘         │
└─────────────────────────────────────────────────────────────────┘
        │ SPMI                    │ External
        ▼                         ▼
┌──────────────┐          ┌──────────────┐
│   PM660      │          │   PM660L     │
│   (PMIC)     │          │   (PMIC)     │
│ VDD_APC      │          │ LDOs for     │
│ VDD_CX/MX    │          │ peripherals  │
│ GPIO/MPP     │          │              │
└──────────────┘          └──────────────┘
```

---

## Key Subsystems Summary

| Subsystem | Component | Key Specs |
|-----------|-----------|-----------|
| **CPU** | Kryo 260 (big.LITTLE) | 4x Gold @ 2.2 GHz + 4x Silver @ 1.8 GHz, ARMv8-A |
| **GPU** | Adreno 509 | OpenGL ES 3.2, Vulkan 1.0 |
| **DSP** | Hexagon 680 | ADSP (audio), CDSP (compute), SLPI (sensors) |
| **Modem** | Snapdragon X12 LTE | Cat 12 DL / Cat 13 UL |
| **Memory** | BIMC | Dual-channel LPDDR4/4x, up to 8 GB |
| **Storage** | UFS 2.1 / eMMC 5.1 | Via SDHCI controller |
| **Display** | DPU + DSI | Dual DSI, up to 2560×1600 |
| **USB** | DWC3 | USB 3.0 Type-C |
| **PMIC** | PM660 + PM660L | SPMI interface, multi-rail |
| **Interconnect** | NoC fabric | SNOC, CNOC, A2NOC, BIMC, MNOC, GNOC |

---

## Boot Sequence Overview

The SDM660 follows Qualcomm's standard secure boot chain:

```
Power On
  │
  ▼
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│   PBL    │───▶│   XBL    │───▶│   ABL    │───▶│  Linux   │
│  (ROM)   │    │  (Flash) │    │   (LK)   │    │  Kernel  │
│          │    │          │    │          │    │          │
│ RSA      │    │ DDR Init │    │ Fastboot │    │ DT Parse │
│ Verify   │    │ Clock    │    │ GPT Read │    │ Drivers  │
│ XBL Load │    │ PMIC     │    │ Kernel   │    │ Init     │
│          │    │ TZ Init  │    │ Load     │    │          │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
                     │                                │
                     ▼                                ▼
               ┌──────────┐                    ┌──────────┐
               │   RPM    │                    │ Android  │
               │ Firmware │                    │  Init    │
               │          │                    │          │
               │ Power    │                    │ Zygote   │
               │ Mgmt     │                    │ SysServer│
               └──────────┘                    └──────────┘
```

Each phase is documented in sequential directories under [BringupDocs/](BringupDocs/):

| Boot Phase | Directory | Key Docs |
|------------|-----------|----------|
| SoC Architecture | [00_SDM660_Architecture/](BringupDocs/00_SDM660_Architecture/) | CPU, memory, NoC, PMIC, partitions |
| Power-On & PBL | [01_Power_On_Reset/](BringupDocs/01_Power_On_Reset/) | Power sequencing, ROM bootloader |
| XBL/SBL | [02_XBL_Secondary_Bootloader/](BringupDocs/02_XBL_Secondary_Bootloader/) | DDR, clocks, TrustZone |
| RPM | [03_RPM_Firmware/](BringupDocs/03_RPM_Firmware/) | Power manager co-processor |
| ABL/LK | [04_ABL_Android_Bootloader/](BringupDocs/04_ABL_Android_Bootloader/) | Fastboot, kernel loading |
| Linux Kernel | [05_Linux_Kernel_Boot/](BringupDocs/05_Linux_Kernel_Boot/) | Device tree, driver framework |
| Peripherals | [06_Peripheral_Bringup/](BringupDocs/06_Peripheral_Bringup/) | I2C, SPI, UART, USB, display |
| Subsystems | [07_Subsystem_Loading/](BringupDocs/07_Subsystem_Loading/) | PIL, ADSP, CDSP, SLPI, modem |
| IPC | [08_IPC_Mechanisms/](BringupDocs/08_IPC_Mechanisms/) | SMEM, SMSM, QMI, GLINK |
| Android | [09_Android_Init/](BringupDocs/09_Android_Init/) | init.rc, zygote, system_server |

---

## Source Code & Build Setup

See [repo/](repo/) for:
- Google `repo` tool installation
- Qualcomm BSP manifest and source sync
- Android source tree layout
- Kernel source setup and build configuration

---

## Related Project

This bring-up documentation supports the **BMI160 IMU Sensor Integration** project — a full-stack integration of the Bosch BMI160 6-axis IMU sensor on this SDM660 platform. See the [project root README](../ReadMe.Md) for details.
