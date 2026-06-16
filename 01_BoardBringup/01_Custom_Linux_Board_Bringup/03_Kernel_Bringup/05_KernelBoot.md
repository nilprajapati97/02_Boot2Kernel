Sure! Let's break down the **Linux Kernel Bring-Up** process step by step **from scratch**, particularly for custom boards or SoCs like Qualcomm SDM660, i.MX6, or similar embedded boards. This guide assumes **U-Boot bootloader is already working** and we are moving to booting the Linux kernel.

---

## 🐧 1. ✅ Kernel Selection (Mainline vs Vendor)
==========================================================================================================================
---

### 🧱 Goal:

Get a Linux kernel source that supports your board (CPU + peripherals).

### 🎯 Options:

* **Vendor Kernel BSP:** Often provided by SoC vendor (e.g., Qualcomm CodeAurora, NXP, TI). Has board-specific patches.
* **Mainline Kernel:** Official Linux ([https://kernel.org](https://kernel.org)). More stable and long-term but may lack vendor features.

### ✅ Steps:

1. **Download Vendor Kernel**:

   ```bash
   git clone <vendor kernel repo>
   ```

2. **Or Get Mainline**:

   ```bash
   git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
   cd linux
   git checkout v6.6
   ```

3. **Check supported SoC/board in `arch/arm/boot/dts/` or `arch/arm64/boot/dts/`**

---

## 🧰 2. ✅ Kernel Configuration
==========================================================================================================================

### 🧱 Goal:

Configure the kernel for your board and peripherals.

### ✅ Steps:

#### 2.1 Select Board Defconfig:

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- <board_defconfig>
```

Examples:

* `make defconfig`
* `make qcom_defconfig` for Qualcomm SoCs
* `make imx_v8_defconfig` for i.MX8

#### 2.2 Customize with `menuconfig`:

```bash
make ARCH=arm64 menuconfig
```

Enable:

* I2C, SPI, GPIO, Ethernet, USB, SDMMC
* File systems (ext4, NFS)
* Root filesystem support (initramfs, block devices)

Save `.config` for repeatable builds.

---

## 🐾 3. ✅ Enable Early Printk / Earlycon

---

### 🧱 Goal:

See kernel boot logs over UART early (even before `printk()` works).

### ✅ Config:

In `menuconfig`, enable:

```bash
CONFIG_EARLY_PRINTK=y
CONFIG_SERIAL_EARLYCON=y
```

### ✅ Add bootargs in U-Boot or DTS:

```bash
earlycon=uart8250,mmio32,0x<uart_base_address>
console=ttyS0,115200n8
```

### 📌 Example for SDM660 UART:

```bash
setenv bootargs "earlycon=msm_serial_dm,0x78af000 console=ttyMSM0,115200n8"
```

### ✅ Check:

If you see logs like:

```
[    0.000000] Booting Linux on physical CPU 0x0
```

→ earlycon is working.

---

## 🌳 4. ✅ Device Tree (.dts)
==========================================================================================================================

### 🧱 Goal:

Describe hardware to the Linux kernel (CPU, memory, peripherals).

### ✅ Steps:

#### 4.1 Locate/Copy DTS:

```bash
arch/arm64/boot/dts/qcom/sdm660.dtsi
arch/arm64/boot/dts/qcom/sdm660-mydboard.dts
```

#### 4.2 Add Nodes:

Enable:

* `serial@78af000` (UART0)
* `mmc@7864900` (eMMC or SD)
* `ethernet@xxxxxx`
* `chosen` → to pass `bootargs`

#### 📌 Example:

```dts
chosen {
    bootargs = "earlycon=msm_serial_dm,0x78af000 console=ttyMSM0,115200n8 root=/dev/mmcblk0p2 rw";
};

&uart0 {
    status = "okay";
};

&sdhc_1 {
    status = "okay";
};
```

#### 4.3 Compile DTB:

```bash
make ARCH=arm64 dtbs
```

---

## 🐧 5. ✅ Kernel Boot & Root Filesystem
==========================================================================================================================

### 🧱 Goal:

Boot kernel and handoff to rootfs (e.g., on SD card or NFS).

### ✅ Steps:

#### 5.1 Prepare Root Filesystem:

* Option 1: **BusyBox initramfs** (for quick test)
* Option 2: **Debian/Ubuntu rootfs** on SD card
* Option 3: **NFS RootFS** (for dev)

Example `bootargs` for NFS:

```bash
root=/dev/nfs nfsroot=192.168.1.100:/nfsroot ip=dhcp
```

#### 5.2 Boot with U-Boot:

```bash
tftpboot 0x80000000 Image
tftpboot 0x86000000 myboard.dtb
booti 0x80000000 - 0x86000000
```

#### 5.3 Expected Output:

```
[    0.000000] Booting Linux...
...
[    2.000000] Freeing init memory: 128K
Welcome to Buildroot or Debian
```

---

## 🛠️ Common Debugging Tips:
==========================================================================================================================
| Symptom                                     | Solution                                         |
| ------------------------------------------- | ------------------------------------------------ |
| No UART log                                 | Check `earlycon`, UART base address, DTS         |
| Kernel hang                                 | Add `initcall_debug`, `ignore_loglevel`          |
| `Kernel panic - not syncing: No init found` | Rootfs missing or wrong `root=`                  |
| Boot loops                                  | Check PMIC reset, watchdog, or missing DT clocks |

---

## ✅ Final Checklist for Bring-Up
==========================================================================================================================
| ✅ Task                                 | Status |
| -------------------------------------- | ------ |
| U-Boot loads `Image` and `.dtb`        | ✅      |
| Kernel starts, shows logs via UART     | ✅      |
| UART, SD, Ethernet nodes enabled in DT | ✅      |
| Root filesystem boots to login shell   | ✅      |

---

Would you like a **sample full working DTS + kernel defconfig + rootfs for SDM660 or i.MX6** for testing on your own board?
