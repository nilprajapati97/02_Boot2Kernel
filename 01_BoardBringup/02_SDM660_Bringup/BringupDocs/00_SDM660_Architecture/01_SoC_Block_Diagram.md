# SDM660 SoC Block Diagram

## Overview

The Qualcomm SDM660 (Snapdragon 660) is a **System-on-Chip (SoC)** — a single silicon die integrating CPU, GPU, DSP, modem, memory controllers, and peripheral interfaces. Understanding the block diagram is the foundation for all bring-up work.

---

## Logical Block Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                            SDM660 SoC (14nm FinFET)                     │
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                     APPLICATION PROCESSOR                       │   │
│  │                                                                 │   │
│  │   ┌─────────────────────┐    ┌─────────────────────┐           │   │
│  │   │   GOLD CLUSTER       │    │   SILVER CLUSTER     │           │   │
│  │   │   (Performance)      │    │   (Efficiency)       │           │   │
│  │   │                     │    │                     │           │   │
│  │   │  CPU0  CPU1  CPU2  │    │  CPU4  CPU5  CPU6  │           │   │
│  │   │  CPU3              │    │  CPU7              │           │   │
│  │   │  Kryo 260 Gold     │    │  Kryo 260 Silver   │           │   │
│  │   │  @ 2.2 GHz         │    │  @ 1.8 GHz         │           │   │
│  │   │  L2 Cache: 1 MB    │    │  L2 Cache: 1 MB    │           │   │
│  │   └────────┬────────────┘    └────────┬───────────┘           │   │
│  │            │                          │                       │   │
│  │            └──────────┬───────────────┘                       │   │
│  │                       │                                       │   │
│  │              ┌────────▼────────┐                              │   │
│  │              │   GIC-400       │  ARM Generic Interrupt        │   │
│  │              │   Interrupt     │  Controller                   │   │
│  │              │   Controller    │                               │   │
│  │              └────────┬────────┘                              │   │
│  └───────────────────────┼──────────────────────────────────────┘   │
│                          │                                           │
│  ┌───────────────────────▼──────────────────────────────────────┐   │
│  │              NETWORK-ON-CHIP (NoC) INTERCONNECT               │   │
│  │                                                               │   │
│  │   GNOC ──── SNOC ──── CNOC ──── A2NOC ──── MNOC ──── BIMC   │   │
│  │  (Global)  (System)  (Config)  (Apps)   (Media)   (Memory)   │   │
│  └──┬────┬──────┬────────┬─────────┬────────┬─────────┬────────┘   │
│     │    │      │        │         │        │         │             │
│     │    │      │        │         │        │         │             │
│  ┌──▼──┐ │   ┌──▼──┐  ┌──▼──┐  ┌──▼──┐  ┌──▼──┐  ┌──▼──────┐   │
│  │Adreno│ │   │ RPM │  │TLMM │  │ QUP │  │ DPU │  │  BIMC    │   │
│  │ 509  │ │   │     │  │Pin  │  │I2C/ │  │Disp │  │  Memory  │   │
│  │ GPU  │ │   │Power│  │Ctrl │  │SPI/ │  │Proc │  │  Ctrl    │   │
│  └──────┘ │   │Mgr  │  │     │  │UART │  │Unit │  │          │   │
│           │   └─────┘  └─────┘  └─────┘  └─────┘  │ DDR4     │   │
│  ┌────────▼────────┐                               │ Dual Ch  │   │
│  │ DSP SUBSYSTEMS  │  ┌─────┐  ┌─────┐  ┌──────┐  └──────────┘   │
│  │                 │  │ USB │  │ UFS │  │Camera│                   │
│  │ ┌─────┐ ┌─────┐│  │DWC3 │  │/eMMC│  │ ISP  │                   │
│  │ │ADSP │ │CDSP ││  │     │  │SDHCI│  │      │                   │
│  │ │Audio│ │Comp ││  └─────┘  └─────┘  └──────┘                   │
│  │ └─────┘ └─────┘│                                               │
│  │ ┌─────┐        │  ┌─────────────┐  ┌──────────┐               │
│  │ │SLPI │        │  │   Modem     │  │  Venus   │               │
│  │ │Sensr│        │  │ Snapdragon  │  │  Video   │               │
│  │ └─────┘        │  │ X12 LTE    │  │ Enc/Dec  │               │
│  └─────────────────┘  └─────────────┘  └──────────┘               │
│                                                                     │
│  ┌────────────────────────────────────────────────────────────┐     │
│  │                    SECURITY SUBSYSTEM                      │     │
│  │   TrustZone (EL3)  │  QSEE (Secure World)  │  Crypto HW  │     │
│  └────────────────────────────────────────────────────────────┘     │
│                                                                     │
│         │ SPMI Bus                         │ GPIO / Interrupts      │
└─────────┼──────────────────────────────────┼───────────────────────┘
          │                                  │
  ┌───────▼───────┐                  ┌───────▼───────┐
  │    PM660      │                  │    PM660L     │
  │   (PMIC)      │                  │   (PMIC)      │
  │               │                  │               │
  │ • Buck/LDO    │                  │ • LDOs        │
  │ • VDD_APC     │                  │ • BOB         │
  │ • VDD_CX/MX   │                  │ • Flash LED   │
  │ • RTC         │                  │ • WLED        │
  │ • PON/KPDPWR  │                  │               │
  └───────────────┘                  └───────────────┘
```

---

## Signal Flow During Boot

Understanding how signals flow through the SoC during boot:

```
1. PMIC Power-Up
   PM660 asserts KPDPWR → SDM660 POR (Power-On Reset)
        │
        ▼
2. PBL (ROM) Executes on CPU0 (Gold Cluster)
   Reads XBL from UFS/eMMC via SDHCI → validates RSA signature
        │
        ▼
3. XBL Runs on CPU0
   Initializes: DDR (via BIMC) → Clocks (via GCC) → PMIC (via SPMI)
   Loads TrustZone → RPM firmware
        │
        ▼
4. ABL/LK Runs on CPU0
   Reads GPT → Loads kernel + DTB from boot partition
   Jumps to kernel entry (head.S)
        │
        ▼
5. Linux Kernel Boots on CPU0
   Parses DT → Initializes GIC → Probes clock/pinctrl/regulators
   Brings up remaining CPUs via PSCI → Mounts rootfs
        │
        ▼
6. Android Init (PID 1)
   Starts services → Loads DSP firmware (PIL) → Launches Zygote
```

---

## Address Map (Key Regions)

| Region | Base Address | Size | Description |
|--------|-------------|------|-------------|
| GCC | 0x0010_0000 | 592 KB | Global Clock Controller |
| TLMM | 0x0101_0000 | 3 MB | Pin Control (Top Level Mode Mux) |
| BLSP1 QUP | 0x078B_5000 | varies | I2C/SPI controllers |
| BLSP1 UART | 0x078A_F000 | 512 B | UART controller |
| USB3 DWC3 | 0x0A80_0000 | varies | USB 3.0 controller |
| UFS | 0x01DA_4000 | varies | UFS host controller |
| BIMC | 0x0040_0000 | varies | Memory controller |
| MMCC | 0x01D0_0000 | varies | Multimedia clock controller |
| GPUCC | 0x0506_5000 | 36 KB | GPU clock controller |
| SMEM | 0x8600_0000 | 4-8 MB | Shared memory (in DDR) |
| RPM MSG RAM | 0x0077_8000 | varies | RPM message RAM |

---

## Power Domains

The SoC has multiple power domains that can be independently controlled:

```
VDD_APC ──── Application Processor Cores (CPU)
VDD_CX  ──── Core logic (always-on digital logic)
VDD_MX  ──── Memory (SRAM retention, caches)
VDD_GFX ──── GPU (Adreno 509)
VDD_LPI_CX ── Low Power Island CX (SLPI)
VDD_LPI_MX ── Low Power Island MX (SLPI)
VDD_DDR ──── External DDR memory
VDD_MSS ──── Modem subsystem
```

During deep sleep, only VDD_CX and VDD_MX remain powered for memory retention, while VDD_APC (CPU) is collapsed.

---

## Related Documents

- [02_CPU_Subsystem.md](02_CPU_Subsystem.md) — Deep-dive into CPU cores
- [03_Memory_Subsystem.md](03_Memory_Subsystem.md) — DDR and memory architecture
- [04_Interconnect_NoC.md](04_Interconnect_NoC.md) — Network-on-Chip details
- [05_PMIC_PM660.md](05_PMIC_PM660.md) — Power management IC
- [06_Partition_Layout.md](06_Partition_Layout.md) — Storage partition scheme
