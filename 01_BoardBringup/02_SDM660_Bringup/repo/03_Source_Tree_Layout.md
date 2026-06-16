# Android Source Tree Layout — SDM660 BSP

## Overview

After syncing the Qualcomm BSP, you have a massive Android source tree. Understanding its layout is essential for bring-up work — knowing where to find kernel sources, device configs, HAL implementations, and vendor blobs.

---

## Top-Level Directory Structure

```
~/sdm660_pie/
├── art/                    # Android Runtime (ART/Dalvik)
├── bionic/                 # C library (libc, libm, libdl)
├── bootable/               # Bootloader (LK) and recovery
│   ├── bootloader/lk/      # Little Kernel bootloader source
│   └── recovery/           # Recovery mode implementation
├── build/                  # Build system (Make/Soong/Blueprint)
│   ├── make/               # Main Makefile system
│   ├── soong/              # Soong build system (Go-based)
│   └── blueprint/          # Blueprint build file processor
├── compatibility/          # CTS/VTS compatibility definitions
├── cts/                    # Compatibility Test Suite
├── dalvik/                 # Dalvik VM (legacy)
├── device/                 # *** DEVICE-SPECIFIC CONFIGS ***
│   └── qcom/
│       ├── sdm660_64/      # SDM660 device tree
│       ├── common/         # Common Qualcomm device configs
│       └── sepolicy/       # SELinux policies
├── external/               # Third-party open-source projects
├── frameworks/             # Android framework
│   ├── base/               # Core framework (Java/C++)
│   ├── native/             # Native framework (SurfaceFlinger, etc.)
│   ├── av/                 # Audio/Video framework
│   └── hardware/           # Hardware abstraction interfaces
├── hardware/               # *** HAL IMPLEMENTATIONS ***
│   ├── interfaces/         # HIDL interface definitions (.hal)
│   ├── libhardware/        # Legacy HAL headers
│   └── qcom/               # Qualcomm HAL implementations
│       ├── audio/          # Audio HAL
│       ├── display/        # Display HAL (HWC, gralloc)
│       ├── camera/         # Camera HAL
│       ├── media/          # Media codecs
│       ├── sensors/        # Sensor HAL
│       ├── bt/             # Bluetooth HAL
│       ├── wlan/           # WiFi HAL
│       └── gps/            # GPS HAL
├── kernel/                 # *** KERNEL SOURCE ***
│   └── msm-4.4/            # Qualcomm kernel (Linux 4.4.x)
├── libcore/                # Core Java libraries
├── packages/               # Android apps & services
├── prebuilts/              # Prebuilt toolchains & binaries
│   ├── clang/              # Clang compiler
│   ├── gcc/                # GCC cross-compiler
│   └── misc/               # Misc tools
├── system/                 # Core system components
│   ├── core/               # Init, adb, logd, toolbox
│   ├── bt/                 # Bluetooth system
│   ├── extras/             # System utilities
│   └── sepolicy/           # SELinux base policy
├── test/                   # Test frameworks
├── tools/                  # Development tools
├── vendor/                 # *** VENDOR CODE ***
│   └── qcom/
│       ├── opensource/      # Open-source Qualcomm code
│       │   ├── core-utils/
│       │   ├── interfaces/
│       │   ├── bluetooth/
│       │   └── wlan/
│       └── proprietary/     # Proprietary blobs (NDA)
│           ├── common/
│           └── prebuilt_HY11/
├── out/                    # *** BUILD OUTPUT ***
│   └── target/product/sdm660_64/
├── Makefile                # Top-level makefile
└── .repo/                  # Repo metadata
```

---

## Key Directories for Bring-Up Engineers

### 1. Device Configuration: `device/qcom/sdm660_64/`

This is the **most critical directory** for board bring-up. It contains:

```
device/qcom/sdm660_64/
├── AndroidBoard.mk         # Board-level build rules
├── AndroidProducts.mk      # Declares build targets (lunch entries)
├── BoardConfig.mk          # *** CRITICAL: Board configuration ***
│                            # - Kernel config, partition sizes
│                            # - Boot image settings
│                            # - SELinux, HIDL configs
├── device.mk               # Device-level makefile
│                            # - Lists packages to include
│                            # - Properties, overlays
├── sdm660.mk               # Product makefile
├── init.target.rc           # Device-specific init scripts
├── init.qcom.rc             # Qualcomm init scripts
├── fstab.qcom               # Filesystem mount table
├── ueventd.qcom.rc          # Device node permissions
├── overlay/                 # Resource overlays
├── sepolicy/                # Device-specific SELinux policies
└── recovery/                # Recovery-specific configs
```

#### Key File: `BoardConfig.mk`

```makefile
# Kernel configuration
TARGET_KERNEL_VERSION := 4.4
BOARD_KERNEL_CMDLINE := console=ttyMSM0,115200n8 androidboot.hardware=qcom
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_KERNEL_IMAGE_NAME := Image.gz-dtb
TARGET_KERNEL_CONFIG := sdm660-perf_defconfig

# Partition sizes
BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864          # 64 MB
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3221225472      # 3 GB
BOARD_VENDORIMAGE_PARTITION_SIZE := 1073741824      # 1 GB
BOARD_USERDATAIMAGE_PARTITION_SIZE := 48318382080

# Platform
TARGET_BOARD_PLATFORM := sdm660
TARGET_BOOTLOADER_BOARD_NAME := sdm660

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
```

### 2. Kernel Source: `kernel/msm-4.4/`

```
kernel/msm-4.4/
├── arch/arm64/
│   ├── boot/dts/qcom/       # *** DEVICE TREES ***
│   │   ├── sdm660.dtsi       # Base SoC device tree
│   │   ├── sdm660-mtp.dts    # MTP board device tree
│   │   ├── pm660.dtsi         # PMIC device tree
│   │   └── sdm660-pinctrl.dtsi  # Pin control definitions
│   ├── configs/
│   │   └── sdm660-perf_defconfig  # *** KERNEL CONFIG ***
│   └── kernel/
│       ├── head.S             # Kernel entry point (assembly)
│       └── setup.c            # Early kernel setup
├── drivers/
│   ├── clk/qcom/             # Qualcomm clock drivers
│   │   └── gcc-sdm660.c      # GCC clock controller
│   ├── pinctrl/qcom/         # Qualcomm pinctrl drivers
│   │   └── pinctrl-sdm660.c  # TLMM pin controller
│   ├── soc/qcom/             # Qualcomm SoC drivers
│   │   ├── smem.c            # Shared memory
│   │   ├── smp2p.c           # SMP2P IPC
│   │   ├── smd.c             # SMD transport
│   │   └── mdt_loader.c      # PIL firmware loader
│   ├── i2c/busses/           # I2C bus drivers
│   │   └── i2c-qup.c         # QUP I2C driver
│   ├── spi/                  # SPI drivers
│   │   └── spi-qup.c         # QUP SPI driver
│   ├── usb/dwc3/             # USB DWC3 controller
│   ├── gpu/drm/msm/          # Adreno GPU (DRM/KMS)
│   ├── iio/imu/              # IMU sensor drivers (BMI160 here)
│   └── regulator/            # Voltage regulators
│       └── qcom_smd-regulator.c
├── include/
│   ├── dt-bindings/          # Device tree binding constants
│   └── linux/
└── Documentation/
    └── devicetree/bindings/  # DT binding documentation
```

### 3. HAL Implementations: `hardware/qcom/`

```
hardware/qcom/
├── display/sdm/              # Display HAL (SDM = Snapdragon Display Manager)
│   ├── sdm660/               # SDM660-specific display
│   ├── libs/hwc2/            # HWComposer 2.0
│   └── gralloc/              # Graphics memory allocator
├── audio/
│   ├── hal/                  # Audio HAL implementation
│   └── configs/sdm660/       # Audio configs
├── sensors/
│   └── hal/                  # Sensor HAL (for BMI160 integration)
├── camera/
│   ├── QCamera2/             # Camera HAL v2
│   └── mm-camera/            # Camera media module
└── media/
    └── mm-video-v4l2/        # Video encoder/decoder
```

### 4. Vendor Blobs: `vendor/qcom/`

```
vendor/qcom/
├── opensource/
│   ├── core-utils/           # Init scripts, property setup
│   ├── interfaces/           # HIDL service definitions
│   ├── wlan/                 # WiFi driver/HAL
│   └── bluetooth/            # BT driver/HAL
└── proprietary/              # Closed-source (NDA)
    ├── common/libs/          # Shared libraries
    ├── prebuilt_HY11/        # Prebuilt binaries
    ├── sensors-see/          # Sensor SEE framework
    └── dspservices/          # DSP daemon services
```

---

## Build Output: `out/target/product/sdm660_64/`

After a successful build:

```
out/target/product/sdm660_64/
├── boot.img                  # Kernel + ramdisk (flash to boot partition)
├── system.img                # /system partition
├── vendor.img                # /vendor partition
├── userdata.img              # /data partition
├── recovery.img              # Recovery image
├── dtbo.img                  # Device tree blob overlay
├── vbmeta.img                # Android Verified Boot metadata
├── ramdisk.img               # Initial RAM disk
├── obj/                      # Intermediate objects
│   ├── KERNEL_OBJ/           # Kernel build objects
│   │   ├── vmlinux           # Uncompressed kernel (for debugging)
│   │   ├── .config           # Active kernel config
│   │   └── arch/arm64/boot/
│   │       ├── Image.gz      # Compressed kernel
│   │       └── dts/qcom/     # Compiled DTBs
│   └── SHARED_LIBRARIES/     # Built shared libraries
├── system/                   # Unpacked system partition
│   ├── bin/                  # System binaries
│   ├── lib64/                # 64-bit libraries
│   └── framework/            # Java framework JARs
├── vendor/                   # Unpacked vendor partition
│   ├── bin/hw/               # HAL service binaries
│   ├── lib64/hw/             # HAL shared libraries
│   └── firmware/             # Firmware blobs
└── root/                     # Root filesystem (ramdisk)
    ├── init                  # Init binary
    ├── init.rc               # Main init script
    └── fstab.qcom            # Mount table
```

---

## Finding Things Quickly

### Common Search Patterns

```bash
# Find device tree for SDM660
find . -name "sdm660*.dts*" -path "*/arch/*"

# Find kernel defconfig
find . -name "*sdm660*defconfig"

# Find a HAL implementation
find hardware/qcom -name "*.cpp" | grep -i sensor

# Find init scripts for device
find device/qcom/sdm660* -name "init*.rc"

# Find where a kernel config is set
grep -r "CONFIG_BMI160" kernel/msm-4.4/

# Find DT binding docs
find kernel/msm-4.4/Documentation/devicetree -name "*i2c*"
```

---

## Next Steps

- [04_Kernel_Source_Setup.md](04_Kernel_Source_Setup.md) — Kernel build configuration and compilation
- [../BringupDocs/00_SDM660_Architecture/](../BringupDocs/00_SDM660_Architecture/) — Start the bring-up documentation
