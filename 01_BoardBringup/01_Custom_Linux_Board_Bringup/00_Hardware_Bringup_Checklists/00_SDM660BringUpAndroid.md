Since you are working on a **custom development board** based on **Qualcomm Snapdragon 660 (SDM660)**, here's a **tailored bring-up checklist and kernel porting guide**, starting from scratch.

This guide assumes:

* You received a **bare-metal dev board (no Linux running yet)**.
* You aim to bring up **U-Boot or Qualcomm's LK**, **Linux kernel**, and **Android/Linux OS**.

---

## ✅ **📋 Bring-Up Checklist for SDM660 Custom Board**

---

### 🔧 1. **Hardware Pre-Checks**

| Item                  | Details                                                                |
| --------------------  | ---------------------------------------------------------------------- |
| ✅ Power Rails        | Validate VDD\_CPU, VDD\_IO, VDD\_MEM using PMIC.                       |
| ✅ Clock Sources      | Ensure XO (e.g., 19.2 MHz) crystal feeding SoC, PLLs configured.       |
| ✅ DDR Initialization | Confirm DDR topology (LPDDR4), timing configuration matches design.    |
| ✅ UART Debug Access  | Expose UART console for logs.                                          |
| ✅ JTAG/SWD           | Optionally wire JTAG for low-level debug (via Lauterbach or QDLoader). |
| ✅ Boot Switches      | Set correct boot config (e.g., eMMC/SD/USB boot).                      |

---

### 🧭 2. **Boot Chain Bring-Up (Qualcomm-Specific)**

| Stage                           | Description                                                                    |
| ------------------------------- | ------------------------------------------------------------------------------ |
| ✅ XBL (eXtendable Bootloader)   | Qualcomm-provided, initializes DDR, PMIC, security. Delivered as signed blob.  |
| ✅ ABL (Android Bootloader) / LK | Qualcomm’s bootloader (like U-Boot). Configures display, partitions, fastboot. |
| ✅ Fastboot Mode                 | Ensure board can enter fastboot mode via USB (ADB/Fastboot tool).              |
| ✅ Console Access                | Validate log output via UART (early boot log).                                 |
| ✅ Partition Table               | Use GPT with `gpt_both0.bin` (Qualcomm), includes boot/recovery/system/vendor. |

---

### 🐧 3. **Linux Kernel Porting (for SDM660)**

#### 🧩 Sources Needed:

* **Qualcomm CAF kernel sources**: (e.g., `LA.UM.7.6.2.r1-XXXX` tag for SDM660)
* Device Tree: `arch/arm64/boot/dts/qcom/sdm660.dtsi`
* Boot Image Format: Android boot image (kernel + DTB + ramdisk)

---

### ⚙️ Porting Steps:

#### Step 1: **Get Kernel Source**

```bash
git clone https://source.codeaurora.org/quic/la/kernel/msm-4.14
git checkout LA.UM.7.6.2.r1-XXX
```

#### Step 2: **Configure Kernel**

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-android-
make sdm660_defconfig   # Or use a board-specific defconfig
```

#### Step 3: **Device Tree and Board Support**

* Create board-specific DTS:

  * `sdm660-yourboard.dts`
  * Include base: `#include "sdm660.dtsi"`
* Add your board to `Makefile` and `Kconfig`.

#### Example DTS:

```dts
/dts-v1/;
#include "sdm660.dtsi"

&uart6 {
    status = "okay";
};

&blsp1_i2c2 {
    status = "okay";
    sensor@40 {
        compatible = "mycompany,my-sensor";
        reg = <0x40>;
    };
};
```

---

#### Step 4: **Compile Kernel**

```bash
make -j8 Image.gz-dtb
```

> Output: `arch/arm64/boot/Image.gz-dtb`

---

#### Step 5: **Create Boot Image (for fastboot flashing)**

```bash
mkbootimg --kernel Image.gz-dtb \
  --ramdisk ramdisk.img \
  --dtb sdm660-yourboard.dtb \
  --base 0x80000000 --pagesize 2048 \
  --output boot.img
```

---

### 🚀 4. **Flash and Test**

| Method             | Command                                                           |
| ------------------ | ----------------------------------------------------------------- |
| Flash via Fastboot | `fastboot flash boot boot.img`                                    |
| Boot temporarily   | `fastboot boot boot.img`                                          |
| Debug UART         | Monitor logs using minicom/picocom                                |
| Log issues         | Use earlycon: `earlycon=msm_serial_dm,0xXXXXXXXX console=ttyMSM0` |

---

### 📦 5. **Root Filesystem & Android/Linux OS**

| Step                  | Details                                                      |
| --------------------- | ------------------------------------------------------------ |
| ✅ Android Platform    | Use AOSP + Qualcomm vendor blobs for SDM660.                 |
| ✅ init.rc/configs     | Match UART, display, GPIO config with your DTS.              |
| ✅ Debug FS Boot       | Use AOSP debug images or minimal initramfs (for Linux only). |
| ✅ Peripheral Bring-Up | One by one: I2C, SPI, GPIO, USB, WLAN, Audio, Display.       |

---

### 🔍 6. **Debug Tips**

| Tool              | Usage                           |
| ----------------- | ------------------------------- |
| `dmesg`           | Kernel boot logs                |
| UART              | Early logs and crash analysis   |
| JTAG (Lauterbach) | Debug DDR/boot failures         |
| Fastboot logs     | For ABL bootloader-level issues |
| QPST/QFIL         | Recovery and XBL flashing       |

---

## 🏁 Summary: Phases

| Phase                     | Checklist                                 |
| ------------------------- | ----------------------------------------- |
| 📦 Bootloader             | XBL/ABL ready, fastboot functional        |
| 🐧 Kernel Port            | DTS created, kernel boots with log        |
| 📂 File System            | Minimal rootfs or Android system boots    |
| 🔌 Peripheral Check       | UART, I2C, SPI, USB, Display, GPIO        |
| 🔐 Secure Boot (Optional) | Use Qualcomm signing tools for production |

---

Would you like:

* 📁 A full working `sdm660-yourboard.dts` example?
* 🧪 A bring-up test plan spreadsheet?
* 🔐 Secure boot and QFIL signing process details?

Let me know and I’ll generate them for you!
