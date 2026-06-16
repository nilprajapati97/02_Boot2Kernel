# USB — DWC3 Controller

## Overview

SDM660 includes a **DWC3 (DesignWare Cores USB 3.0)** controller supporting USB 3.0 (5 Gbps) and USB 2.0. It operates in **dual-role** mode: Device mode (for fastboot/ADB) and Host mode (for USB peripherals like keyboards, storage).

---

## USB Architecture

```
┌──────────────────────────────────────────────────────────┐
│                  SDM660 USB Subsystem                      │
│                                                          │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  DWC3 Controller @ 0x0A80_0000                      │ │
│  │                                                     │ │
│  │  ├── USB 3.0 SuperSpeed (5 Gbps)                   │ │
│  │  ├── USB 2.0 High-Speed (480 Mbps)                 │ │
│  │  ├── Dual-role (Device/Host/OTG)                    │ │
│  │  └── Endpoints: 16 IN + 16 OUT                     │ │
│  └─────────────────────┬───────────────────────────────┘ │
│                        │                                  │
│  ┌─────────────────────▼───────────────────────────────┐ │
│  │  Qualcomm USB Wrapper                                │ │
│  │  ├── QUSB2 PHY (USB 2.0 PHY) @ 0x0C01_2000        │ │
│  │  ├── QMP PHY (USB 3.0 PHY) @ 0x0C01_0000          │ │
│  │  ├── Type-C / CC logic                             │ │
│  │  └── VBUS detection                                │ │
│  └─────────────────────┬───────────────────────────────┘ │
│                        │                                  │
│                   USB Connector                           │
└──────────────────────────────────────────────────────────┘
```

---

## USB Modes

```
Device Mode (gadget):
─────────────────────
Used for: ADB, fastboot, MTP, PTP, tethering
USB connector → SDM660 is the device
Host PC sends USB requests

Host Mode:
──────────
Used for: USB keyboard, mouse, flash drive, Ethernet
USB connector → SDM660 is the host (provides VBUS 5V)
SDM660 sends USB requests

OTG (On-The-Go):
─────────────────
ID pin detection → automatically switch between host/device
Or Type-C CC pin logic for role detection
```

---

## Device Tree

```dts
usb3: ssusb@a800000 {
    compatible = "qcom,dwc-usb3-msm";
    reg = <0x0a800000 0xfc100>;
    interrupts = <GIC_SPI 131 IRQ_TYPE_LEVEL_HIGH>;

    clocks = <&gcc GCC_USB30_MASTER_CLK>,
             <&gcc GCC_USB30_SLEEP_CLK>,
             <&gcc GCC_USB30_MOCK_UTMI_CLK>,
             <&gcc GCC_USB_PHY_CFG_AHB2PHY_CLK>;
    clock-names = "core_clk", "sleep_clk", "mock_utmi_clk",
                  "cfg_ahb_clk";

    vdd-supply = <&pm660l_s4>;
    vdda33-supply = <&pm660l_l7>;
    vdda18-supply = <&pm660_l11>;

    dwc3@a800000 {
        compatible = "snps,dwc3";
        reg = <0x0a800000 0xcd00>;
        interrupts = <GIC_SPI 131 IRQ_TYPE_LEVEL_HIGH>;
        dr_mode = "otg";           /* Dual-role */
        maximum-speed = "super-speed"; /* USB 3.0 */
        snps,has-lpm-erratum;
        snps,hird-threshold = /bits/ 8 <0x10>;
    };
};
```

---

## USB Gadget (ADB/Fastboot)

```bash
# Check USB configuration
adb shell cat /sys/class/android_usb/android0/state
# CONFIGURED

# USB functions
adb shell cat /sys/class/android_usb/android0/functions
# adb

# Enable MTP + ADB
adb shell setprop sys.usb.config mtp,adb
```

---

## Debugging USB

```bash
# USB device info
adb shell cat /sys/kernel/debug/usb/devices

# DWC3 debug
adb shell cat /sys/kernel/debug/a800000.dwc3/mode
# device / host

# USB PHY status
adb shell dmesg | grep -i "usb\|dwc3\|phy"

# On host PC (Linux)
lsusb
# Bus 001 Device 010: ID 05c6:9091 Qualcomm, Inc.
```

### Common USB Issues

| Problem | Symptom | Fix |
|---------|---------|-----|
| No USB enumeration | PC doesn't see device | Check VBUS, USB PHY clocks |
| Stuck in USB 2.0 | Slow transfer speed | Check QMP PHY (USB 3.0 PHY) init |
| ADB not connecting | "no devices" on host | Check USB gadget config, android_usb |
| Fastboot not working | Stuck at fastboot logo | Check DWC3 device mode, USB cable |

---

## Related Documents

- [../04_ABL_Android_Bootloader/01_LK_Little_Kernel.md](../04_ABL_Android_Bootloader/01_LK_Little_Kernel.md) — Fastboot uses USB
- [../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md](../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md) — USB clocks
