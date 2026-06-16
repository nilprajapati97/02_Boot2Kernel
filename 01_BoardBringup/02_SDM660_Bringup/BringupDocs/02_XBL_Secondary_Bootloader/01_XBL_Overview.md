# XBL (eXtensible Boot Loader) — Overview

## Overview

**XBL (eXtensible Boot Loader)** is the second-stage bootloader on Qualcomm platforms, replacing the older SBL (Secondary Boot Loader). It is the first **updatable** boot code — loaded from the `xbl` flash partition by PBL. XBL performs the heavy lifting of hardware initialization before handing off to the Android bootloader.

---

## XBL vs SBL

| Feature | SBL (Legacy) | XBL (SDM660+) |
|---------|-------------|----------------|
| Framework | Proprietary | UEFI-based (EDK2) |
| Modularity | Monolithic | Modular (drivers, libraries) |
| Debug | Limited | UEFI Shell, serial debug |
| Config | Hardcoded | `xbl_config` partition |
| Source | Qualcomm proprietary | Qualcomm proprietary (NDA) |

---

## What XBL Does

```
PBL jumps to XBL entry point
    │
    ▼
┌───────────────────────────────────────────────────────┐
│                    XBL Execution                       │
│                                                       │
│  Phase 1: Early Init (IMEM only, no DDR)              │
│  ──────────────────────────────────────               │
│  ├── Initialize UART for debug output                 │
│  ├── Read xbl_config for board-specific settings     │
│  ├── Initialize clock tree (GCC PLLs)                │
│  ├── Initialize SPMI → Configure PMIC               │
│  └── Initialize DDR controller (BIMC)                │
│      └── Perform DDR training                        │
│      └── *** DDR IS NOW AVAILABLE ***                │
│                                                       │
│  Phase 2: Main Init (DDR available)                   │
│  ──────────────────────────────────                   │
│  ├── Relocate XBL code to DDR (more space)           │
│  ├── Load and authenticate TZ from tz partition      │
│  ├── Initialize TrustZone (secure world setup)       │
│  ├── Load and authenticate RPM firmware              │
│  ├── Load and authenticate Hypervisor (hyp)          │
│  ├── Initialize security: crypto HW, fuse reading    │
│  ├── Read device info: serial number, board ID       │
│  └── Set up SMEM (shared memory) base region         │
│                                                       │
│  Phase 3: Handoff                                     │
│  ──────────────────                                   │
│  ├── Load ABL (Android Boot Loader / LK)             │
│  ├── Authenticate ABL                                │
│  ├── Pass boot parameters via shared memory          │
│  └── Jump to ABL entry point                         │
└───────────────────────────────────────────────────────┘
```

---

## XBL Memory Layout

```
Before DDR Init:
────────────────
┌─────────────────────┐ 0x0014_6000 (IMEM top)
│  XBL Code + Data    │
│  Stack              │
│  PBL shared data    │
└─────────────────────┘ 0x0014_0000 (IMEM base)

After DDR Init:
───────────────
┌─────────────────────┐ DDR Top
│  ...                │
├─────────────────────┤
│  TZ Memory (secure) │  ~1 MB
├─────────────────────┤
│  RPM Code Data      │  ~256 KB
├─────────────────────┤
│  XBL Relocated      │  ~2 MB
├─────────────────────┤
│  SMEM Region        │  ~4 MB
├─────────────────────┤
│  ABL/LK Code        │  ~2 MB
├─────────────────────┤
│  Kernel Load Area   │  (prepared for ABL)
├─────────────────────┤
│  ...                │
└─────────────────────┘ DDR Base (0x8000_0000 typical)
```

---

## XBL Configuration (`xbl_config` partition)

The `xbl_config` partition contains board-specific configuration that XBL reads during early init:

| Config Item | Purpose |
|-------------|---------|
| DDR parameters | Memory type, speed, rank, size |
| PMIC config | Rail voltages, sequencing |
| Board ID | Hardware revision detection |
| Clock config | PLL settings, initial frequencies |
| CDT (Config Data Table) | Comprehensive board config blob |

---

## XBL Debug Output

XBL outputs boot logs via **UART**. This is the earliest serial output available during boot:

```
Format: [XBL] message
Example output:
────────────────
[XBL] Start
[XBL] PMIC Initialized
[XBL] DDR: Freq 1017 MHz, Size 4096 MB, Rank 2
[XBL] DDR Training: PASS
[XBL] TZ Loaded and Authenticated
[XBL] RPM FW Loaded
[XBL] SMEM Initialized
[XBL] ABL Loaded
[XBL] Jumping to ABL...
```

To capture XBL logs, connect a USB-to-UART adapter to the debug UART port (typically BLSP1 UART, pins defined by board schematic).

---

## XBL Source Structure (Qualcomm NDA)

```
boot_images/
├── QcomPkg/
│   ├── SDM660Pkg/           # SDM660-specific package
│   │   ├── Library/
│   │   │   ├── DDRLib/       # DDR initialization library
│   │   │   ├── ClockLib/     # Clock/PLL setup
│   │   │   ├── PmicLib/      # PMIC driver
│   │   │   └── SmemLib/      # Shared memory init
│   │   └── Settings/
│   │       ├── DDR/           # DDR timing parameters
│   │       └── PMIC/          # PMIC config
│   ├── Drivers/
│   │   ├── UFS/               # UFS storage driver
│   │   ├── SDCC/              # eMMC/SD driver
│   │   └── UART/              # Debug UART driver
│   └── Tools/
├── XBLLoader/                 # Main XBL loader code
├── XBLRamDump/               # RAM dump collection
└── XBLConfig/                # Config parser
```

---

## Key XBL Subsystem Initialization (Detailed in Sub-docs)

| Sub-document | XBL Responsibility |
|-------------|-------------------|
| [02_DDR_Init_Training.md](02_DDR_Init_Training.md) | DDR controller setup, PHY training |
| [03_Clock_Tree_Setup.md](03_Clock_Tree_Setup.md) | PLL configuration, initial clock tree |
| [04_PMIC_Regulator_Init.md](04_PMIC_Regulator_Init.md) | Fine-tune voltage rails via SPMI |
| [05_TrustZone_QSEE_Init.md](05_TrustZone_QSEE_Init.md) | Load TZ firmware, secure world setup |

---

## Related Documents

- [../01_Power_On_Reset/02_PBL_Primary_Bootloader.md](../01_Power_On_Reset/02_PBL_Primary_Bootloader.md) — PBL loads XBL
- [../03_RPM_Firmware/01_RPM_Overview.md](../03_RPM_Firmware/01_RPM_Overview.md) — RPM firmware loaded by XBL
- [../04_ABL_Android_Bootloader/01_LK_Little_Kernel.md](../04_ABL_Android_Bootloader/01_LK_Little_Kernel.md) — ABL loaded by XBL
