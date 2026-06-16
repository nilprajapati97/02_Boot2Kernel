# eMMC & UFS Storage

## Overview

SDM660 supports two types of internal flash storage: **eMMC (embedded Multi-Media Card)** and **UFS (Universal Flash Storage)**. Most SDM660-based devices use UFS 2.1 for its significantly higher performance.

---

## UFS vs eMMC

| Feature | eMMC 5.1 | UFS 2.1 |
|---------|----------|---------|
| Interface | Parallel (8-bit) | Serial (2 lanes) |
| Speed | Up to 400 MB/s | Up to 1200 MB/s |
| Full Duplex | No | Yes |
| Queue Depth | 1 | 32 (command queue) |
| Power | Higher | Lower (lane gating) |
| Controller | SDCC (SD/eMMC Host) | UFS Host Controller |

---

## UFS Architecture on SDM660

```
┌──────────────────────────────────────────────────────┐
│  Linux Kernel                                        │
│  ┌────────────────────────────────────────────────┐  │
│  │  Block Layer (bio/request queue)               │  │
│  └──────────────┬─────────────────────────────────┘  │
│                 │                                    │
│  ┌──────────────▼─────────────────────────────────┐  │
│  │  SCSI Layer                                    │  │
│  │  (UFS appears as SCSI device)                  │  │
│  └──────────────┬─────────────────────────────────┘  │
│                 │                                    │
│  ┌──────────────▼─────────────────────────────────┐  │
│  │  UFS Driver (drivers/scsi/ufs/)                │  │
│  │  ├── ufshcd.c     (Host Controller Driver)     │  │
│  │  ├── ufs-qcom.c   (Qualcomm platform glue)    │  │
│  │  └── ufshcd-pltfrm.c (Platform framework)     │  │
│  └──────────────┬─────────────────────────────────┘  │
│                 │ MMIO                               │
│  ┌──────────────▼─────────────────────────────────┐  │
│  │  UFS Host Controller @ 0x01DA4000              │  │
│  │  ├── UFSHCI registers                          │  │
│  │  └── Qualcomm UFS PHY (UniPro/M-PHY)          │  │
│  └──────────────┬─────────────────────────────────┘  │
│                 │ M-PHY lanes                        │
│                 ▼                                    │
│          UFS Flash Device                            │
│          (Samsung/Micron/etc.)                        │
└──────────────────────────────────────────────────────┘
```

---

## Device Tree

```dts
ufshc: ufshc@1da4000 {
    compatible = "qcom,ufshc";
    reg = <0x1da4000 0x3000>;
    interrupts = <GIC_SPI 265 IRQ_TYPE_LEVEL_HIGH>;
    phys = <&ufs_phy>;
    phy-names = "ufsphy";

    clocks = <&gcc GCC_UFS_AXI_CLK>,
             <&gcc GCC_UFS_AHB_CLK>,
             <&gcc GCC_UFS_UNIPRO_CORE_CLK>,
             <&gcc GCC_UFS_ICE_CORE_CLK>;
    clock-names = "core_clk", "iface_clk", "core_clk_unipro",
                  "ice_core_clk";

    vdd-hba-supply = <&pm660l_s4>;
    vcc-supply = <&pm660l_l4>;
    vccq2-supply = <&pm660_l8>;

    lanes-per-direction = <2>;
    status = "okay";
};
```

---

## Storage Partitions

After UFS driver probes, partitions appear as:

```
/dev/block/sda    → UFS LUN 0 (boot partitions: xbl, tz, boot, etc.)
/dev/block/sdb    → UFS LUN 1 (if provisioned)
/dev/block/sda1   → First GPT partition
/dev/block/sda2   → Second GPT partition
...

Symlinks for convenience:
/dev/block/bootdevice/by-name/boot    → /dev/block/sda11
/dev/block/bootdevice/by-name/system  → /dev/block/sda15
/dev/block/bootdevice/by-name/userdata → /dev/block/sda20
```

---

## eMMC (If Used Instead of UFS)

```dts
sdhc_1: sdhci@c0c4000 {
    compatible = "qcom,sdhci-msm-v5";
    reg = <0xc0c4000 0x1000>;
    interrupts = <GIC_SPI 165 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&gcc GCC_SDCC1_APPS_CLK>,
             <&gcc GCC_SDCC1_AHB_CLK>;
    clock-names = "core", "iface";
    bus-width = <8>;
    non-removable;
    status = "okay";
};
```

---

## Debugging Storage

```bash
# UFS info
adb shell cat /sys/class/scsi_host/host0/unique_id
adb shell cat /sys/block/sda/device/model

# UFS speed mode
adb shell cat /sys/bus/platform/devices/1da4000.ufshc/rpm_lvl

# Partition listing
adb shell ls -la /dev/block/bootdevice/by-name/

# I/O performance test
adb shell dd if=/dev/zero of=/data/test bs=1M count=100
```

---

## Related Documents

- [../04_ABL_Android_Bootloader/02_Partition_Table_GPT.md](../04_ABL_Android_Bootloader/02_Partition_Table_GPT.md) — GPT partition layout
- [../00_SDM660_Architecture/06_Partition_Layout.md](../00_SDM660_Architecture/06_Partition_Layout.md) — Partition overview
