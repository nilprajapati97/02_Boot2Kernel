# Kernel Source Setup & Build — SDM660

## Overview

The Linux kernel is the heart of the SDM660 bring-up. Qualcomm provides a heavily modified kernel (Linux 4.4.x for Android 9) with drivers for all SoC subsystems — clock controllers, pin muxing, I2C/SPI/UART, DSP management, power management, and more.

---

## Kernel Source Location

```
kernel/msm-4.4/                   # Root of Qualcomm kernel source
├── arch/arm64/                   # ARM64 architecture code
│   ├── boot/dts/qcom/            # Device Tree Source files
│   ├── configs/                  # Defconfig files
│   │   ├── sdm660-perf_defconfig # *** Production config ***
│   │   └── sdm660_defconfig      # Debug config
│   └── kernel/
│       └── head.S                # Kernel entry point
├── drivers/                      # Device drivers
├── include/                      # Headers
├── init/                         # Kernel init (main.c → start_kernel)
├── mm/                           # Memory management
├── fs/                           # Filesystems
├── net/                          # Networking stack
├── scripts/                      # Build scripts
├── Makefile                      # Top-level kernel Makefile
└── .config                       # Generated config (after make)
```

---

## Kernel Configuration (defconfig)

### SDM660 Defconfig Files

| Defconfig | Purpose |
|-----------|---------|
| `sdm660-perf_defconfig` | Production — optimized for performance, debug off |
| `sdm660_defconfig` | Debug — more logging, debug features enabled |

### Key Configuration Options for Bring-Up

```kconfig
# CPU & Architecture
CONFIG_ARCH_QCOM=y                    # Qualcomm SoC support
CONFIG_ARCH_SDM660=y                  # SDM660 platform
CONFIG_ARM64=y                        # 64-bit ARM
CONFIG_SMP=y                          # Symmetric Multi-Processing
CONFIG_NR_CPUS=8                      # 8 CPU cores
CONFIG_HZ=300                         # Timer frequency

# Clock Framework
CONFIG_COMMON_CLK=y                   # Common clock framework
CONFIG_COMMON_CLK_QCOM=y             # Qualcomm clock driver
CONFIG_SDM_GCC_660=y                  # GCC for SDM660
CONFIG_SDM_GPUCC_660=y               # GPU clock controller
CONFIG_SDM_MMCC_660=y                # Multimedia clock controller

# Pin Control
CONFIG_PINCTRL_SDM660=y              # SDM660 TLMM pin controller

# I2C
CONFIG_I2C=y
CONFIG_I2C_QUP=y                      # Qualcomm QUP I2C driver

# SPI
CONFIG_SPI=y
CONFIG_SPI_QUP=y                      # Qualcomm QUP SPI driver

# Serial/UART
CONFIG_SERIAL_MSM=y                   # Qualcomm UART driver
CONFIG_SERIAL_MSM_CONSOLE=y          # UART console support

# USB
CONFIG_USB_DWC3=y                     # DesignWare USB3 controller
CONFIG_USB_DWC3_MSM=y               # Qualcomm DWC3 glue

# Storage
CONFIG_MMC=y                          # MMC/SD/eMMC support
CONFIG_MMC_SDHCI=y                   # SDHCI controller
CONFIG_SCSI_UFS_QCOM=y              # Qualcomm UFS support

# Display
CONFIG_DRM_MSM=y                     # Qualcomm DRM/KMS driver

# Power Management
CONFIG_REGULATOR=y                    # Voltage regulator framework
CONFIG_REGULATOR_QCOM_SMD_RPM=y     # RPM regulators
CONFIG_QCOM_SCM=y                    # Secure Channel Manager
CONFIG_QCOM_SMEM=y                   # Shared Memory
CONFIG_QCOM_SMP2P=y                  # SMP2P IPC
CONFIG_QCOM_PIL=y                    # Peripheral Image Loader

# Sensors (for BMI160)
CONFIG_IIO=y                          # Industrial I/O subsystem
CONFIG_BMI160=y                       # BMI160 IMU driver
CONFIG_BMI160_I2C=y                  # BMI160 I2C transport

# Debug (enable for bring-up, disable for production)
CONFIG_DYNAMIC_DEBUG=y               # Dynamic debug prints
CONFIG_DEBUG_INFO=y                  # Debug symbols in vmlinux
CONFIG_EARLY_PRINTK=y               # Early kernel messages
CONFIG_SERIAL_EARLYCON=y            # Early console
```

---

## Building the Kernel

### Method 1: Standalone Kernel Build

```bash
cd kernel/msm-4.4

# Set cross-compiler
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-android-
export CLANG_TRIPLE=aarch64-linux-gnu-

# Use prebuilt toolchain from AOSP
export PATH=$PATH:~/sdm660_pie/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin
export PATH=$PATH:~/sdm660_pie/prebuilts/clang/host/linux-x86/clang-r328903/bin

# Generate .config from defconfig
make sdm660-perf_defconfig

# Build kernel image + DTBs
make -j$(nproc) Image.gz-dtb

# Build individual DTB
make -j$(nproc) qcom/sdm660-mtp.dtb

# Build kernel modules (if any configured as =m)
make -j$(nproc) modules
```

### Method 2: Build via Android Build System

```bash
cd ~/sdm660_pie

source build/envsetup.sh
lunch sdm660_64-userdebug

# Build boot image (kernel + ramdisk)
make bootimage -j$(nproc)
```

### Build Outputs

```
# Standalone build outputs:
arch/arm64/boot/Image.gz              # Compressed kernel
arch/arm64/boot/Image.gz-dtb          # Kernel + appended DTB
arch/arm64/boot/dts/qcom/sdm660-mtp.dtb  # Compiled device tree

# Android build outputs:
out/target/product/sdm660_64/kernel    # Kernel binary
out/target/product/sdm660_64/boot.img  # boot.img (kernel + ramdisk)
out/target/product/sdm660_64/obj/KERNEL_OBJ/  # Full kernel build tree
```

---

## Modifying Kernel Configuration

### Interactive Configuration

```bash
cd kernel/msm-4.4

# Start from defconfig
make sdm660-perf_defconfig

# Open menu-based config editor
make menuconfig

# After making changes, save back to defconfig
make savedefconfig
cp defconfig arch/arm64/configs/sdm660-perf_defconfig
```

### Adding a New Driver (Example: BMI160)

```bash
# 1. Enable in defconfig
echo "CONFIG_BMI160=y" >> arch/arm64/configs/sdm660-perf_defconfig
echo "CONFIG_BMI160_I2C=y" >> arch/arm64/configs/sdm660-perf_defconfig

# 2. Regenerate .config
make sdm660-perf_defconfig

# 3. Verify
grep BMI160 .config
# CONFIG_BMI160=y
# CONFIG_BMI160_I2C=y

# 4. Rebuild
make -j$(nproc) Image.gz-dtb
```

---

## Device Tree Compilation

### Compiling DTBs

```bash
# Compile all SDM660 DTBs
make -j$(nproc) dtbs

# Compile specific DTB
make qcom/sdm660-mtp.dtb

# Decompile a DTB for inspection
dtc -I dtb -O dts -o sdm660-mtp.dts arch/arm64/boot/dts/qcom/sdm660-mtp.dtb
```

### DTB Overlay (DTBO) for Android

Android uses **Device Tree Blob Overlay (DTBO)** to separate SoC-level DT from board-level DT:

```
sdm660.dtsi          ← SoC base (provided by Qualcomm)
  └── sdm660-mtp.dts ← Board overlay (customized per board)
       └── sdm660-mtp-overlay.dts  ← DTBO (runtime overlay)
```

```bash
# Build DTBO image
make dtbo.img
```

---

## Kernel Debug Techniques

### Early Console (earlycon)

Add to kernel command line (in `BoardConfig.mk` or ABL):

```
console=ttyMSM0,115200n8 earlycon=msm_serial_dm,0x78af000
```

### Dynamic Debug

```bash
# Enable debug prints for a module at runtime
adb shell "echo 'module i2c_qup +p' > /sys/kernel/debug/dynamic_debug/control"

# Enable debug for a file
adb shell "echo 'file bmi160_core.c +p' > /sys/kernel/debug/dynamic_debug/control"
```

### Kernel Log

```bash
# View kernel log
adb shell dmesg | less

# Filter for specific subsystem
adb shell dmesg | grep -i "i2c\|qup"
adb shell dmesg | grep -i "bmi160"
adb shell dmesg | grep -i "clock\|gcc"
```

### Kernel Symbols (vmlinux)

```bash
# vmlinux location (uncompressed kernel with debug symbols)
ls out/target/product/sdm660_64/obj/KERNEL_OBJ/vmlinux

# Disassemble a function
aarch64-linux-android-objdump -d vmlinux | grep -A 20 "start_kernel"

# Look up symbol address
aarch64-linux-android-nm vmlinux | grep i2c_qup
```

---

## Flashing a New Kernel

```bash
# Method 1: Flash boot.img via fastboot
adb reboot bootloader
fastboot flash boot boot.img
fastboot reboot

# Method 2: Boot without flashing (temporary)
adb reboot bootloader
fastboot boot boot.img
```

---

## Kernel Source References for Bring-Up

| What | Where |
|------|-------|
| Kernel entry point | `arch/arm64/kernel/head.S` |
| Early setup | `init/main.c` → `start_kernel()` |
| Device tree base | `arch/arm64/boot/dts/qcom/sdm660.dtsi` |
| GCC clock driver | `drivers/clk/qcom/gcc-sdm660.c` |
| Pin controller | `drivers/pinctrl/qcom/pinctrl-sdm660.c` |
| I2C QUP driver | `drivers/i2c/busses/i2c-qup.c` |
| SPI QUP driver | `drivers/spi/spi-qup.c` |
| UART driver | `drivers/tty/serial/msm_serial.c` |
| Shared memory | `drivers/soc/qcom/smem.c` |
| PIL loader | `drivers/soc/qcom/mdt_loader.c` |
| SCM interface | `drivers/firmware/qcom_scm.c` |
| USB DWC3 | `drivers/usb/dwc3/dwc3-msm.c` |
| Regulator | `drivers/regulator/qcom_smd-regulator.c` |
| BMI160 IMU | `drivers/iio/imu/bmi160/bmi160_core.c` |

---

## Next Steps

- [../BringupDocs/00_SDM660_Architecture/](../BringupDocs/00_SDM660_Architecture/) — Understand the SoC architecture
- [../BringupDocs/05_Linux_Kernel_Boot/](../BringupDocs/05_Linux_Kernel_Boot/) — How the kernel boots on SDM660
