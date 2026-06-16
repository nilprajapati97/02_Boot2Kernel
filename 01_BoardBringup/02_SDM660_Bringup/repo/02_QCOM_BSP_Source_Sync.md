# Qualcomm BSP Source Sync for SDM660

## Overview

Qualcomm distributes its Android BSP through **CodeAurora Forum (CAF)** — now part of **CodeLinaro**. The BSP includes the Android framework, kernel, device configurations, HALs, and proprietary components needed to bring up an SDM660 board.

---

## Qualcomm Release Naming Convention

Qualcomm BSP releases follow this pattern:

```
LA.UM.<version>.r<revision>-<build_id>-<chipset>.xml

Example: LA.UM.7.2.r1-04900-sdm660.0.xml
```

| Field | Meaning |
|-------|---------|
| `LA` | Linux Android |
| `UM` | User Mode (full Android, not LE = Linux Embedded) |
| `7.2` | Android version mapping (7.2 → Android 9 Pie) |
| `r1` | Revision 1 |
| `04900` | Build ID |
| `sdm660` | Target chipset |

### Android Version Mapping

| LA.UM Version | Android Version |
|---------------|-----------------|
| LA.UM.6.x | Android 8.x (Oreo) |
| LA.UM.7.x | Android 9 (Pie) |
| LA.UM.8.x | Android 10 (Q) |
| LA.UM.9.x | Android 11 (R) |

---

## Step-by-Step Source Sync

### Step 1: Prepare Build Environment

```bash
# Install required packages (Ubuntu 18.04+)
sudo apt-get install -y \
    git-core gnupg flex bison build-essential \
    zip curl zlib1g-dev gcc-multilib g++-multilib \
    libc6-dev-i386 libncurses5 lib32ncurses5-dev \
    x11proto-core-dev libx11-dev lib32z1-dev \
    libgl1-mesa-dev libxml2-utils xsltproc unzip \
    fontconfig python3 openjdk-8-jdk

# Set Java 8 as default
sudo update-alternatives --config java
```

### Step 2: Initialize Repo Workspace

```bash
# Create workspace
mkdir -p ~/sdm660_pie && cd ~/sdm660_pie

# Initialize with Qualcomm BSP manifest
# NOTE: Replace the manifest filename with your specific release
repo init \
    -u https://source.codeaurora.org/quic/la/platform/manifest \
    -b release \
    -m LA.UM.7.2.r1-04900-sdm660.0.xml \
    --depth=1
```

### Step 3: Sync All Source

```bash
# Full sync with 8 threads (takes 1-3 hours depending on network)
repo sync -j8 -c --no-tags --no-clone-bundle

# Verify sync completed
repo status | head -20
```

### Step 4: Verify Critical Directories

```bash
# These directories must exist after sync
ls -d kernel/msm-4.4          # Kernel source
ls -d device/qcom/sdm660_64   # Device configuration
ls -d vendor/qcom              # Qualcomm vendor code
ls -d hardware/qcom            # HAL implementations
```

---

## Manifest Structure for SDM660

A typical SDM660 manifest references these key repositories:

```xml
<!-- Core AOSP Framework -->
<project path="frameworks/base" name="platform/frameworks/base" />
<project path="frameworks/native" name="platform/frameworks/native" />
<project path="system/core" name="platform/system/core" />

<!-- Kernel -->
<project path="kernel/msm-4.4"
         name="kernel/msm-4.4"
         revision="android-9.0" />

<!-- Device Configuration -->
<project path="device/qcom/sdm660_64"
         name="device/qcom/sdm660_64" />
<project path="device/qcom/common"
         name="device/qcom/common" />

<!-- Qualcomm HALs -->
<project path="hardware/qcom/display"
         name="platform/hardware/qcom/sdm660/display" />
<project path="hardware/qcom/audio"
         name="platform/hardware/qcom/sdm660/audio" />
<project path="hardware/qcom/camera"
         name="platform/hardware/qcom/sdm660/camera" />
<project path="hardware/qcom/media"
         name="platform/hardware/qcom/sdm660/media" />

<!-- Vendor Proprietary (if NDA access) -->
<project path="vendor/qcom/proprietary"
         name="vendor/qcom/proprietary" />

<!-- Open Source Qualcomm Components -->
<project path="vendor/qcom/opensource/interfaces"
         name="vendor/qcom/opensource/interfaces" />
<project path="vendor/qcom/opensource/core-utils"
         name="vendor/qcom/opensource/core-utils" />
```

---

## Proprietary vs Open-Source Components

### Open Source (Available via CAF/CodeLinaro)

| Component | Repository Path |
|-----------|-----------------|
| Kernel | `kernel/msm-4.4` |
| Device config | `device/qcom/sdm660_64` |
| Display HAL | `hardware/qcom/display` |
| Audio HAL | `hardware/qcom/audio` |
| WiFi driver | `vendor/qcom/opensource/wlan` |
| BT stack | `vendor/qcom/opensource/bluetooth` |

### Proprietary (Requires Qualcomm NDA/License)

| Component | Description |
|-----------|-------------|
| `vendor/qcom/proprietary` | Closed-source libraries, firmware |
| Hexagon SDK | DSP development tools |
| ADSP/CDSP/SLPI firmware | `.mdt` + `.bXX` firmware blobs |
| Modem firmware | Baseband processor firmware |
| TrustZone (TZ) | Secure world firmware |
| Camera tuning | ISP tuning parameters |

### Getting Proprietary Blobs

For production boards, proprietary blobs are typically:
1. Provided by Qualcomm under NDA as part of BSP delivery
2. Extracted from a running device: `adb pull /vendor/lib/`, `/vendor/firmware/`
3. Available in pre-built form in `vendor/qcom/proprietary/prebuilt_HY11`

---

## Build Configuration

### Step 5: Set Up Build Environment

```bash
cd ~/sdm660_pie

# Initialize build environment
source build/envsetup.sh

# Select target
# For SDM660 MTP (Mobile Test Platform):
lunch sdm660_64-userdebug
```

### Common Build Targets for SDM660

| Target | Description |
|--------|-------------|
| `sdm660_64-userdebug` | 64-bit SDM660, debug enabled |
| `sdm660_64-user` | 64-bit SDM660, production |
| `sdm660_32-userdebug` | 32-bit SDM660, debug enabled |

### Step 6: Build

```bash
# Full build (takes 2-6 hours on first build)
make -j$(nproc)

# Build specific image
make bootimage -j$(nproc)     # Kernel + ramdisk
make systemimage -j$(nproc)   # System partition
make vendorimage -j$(nproc)   # Vendor partition
```

### Build Output

```
out/target/product/sdm660_64/
├── boot.img            # Kernel + ramdisk
├── system.img          # System partition image
├── vendor.img          # Vendor partition image
├── userdata.img        # Userdata partition image
├── dtbo.img            # Device Tree Blob Overlay
├── vbmeta.img          # Verified boot metadata
└── ramdisk.img         # Initial ramdisk
```

---

## Flashing Images

```bash
# Enter fastboot mode
adb reboot bootloader

# Flash all images
fastboot flash boot boot.img
fastboot flash system system.img
fastboot flash vendor vendor.img
fastboot flash userdata userdata.img

# Or flash all at once
fastboot flashall
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `repo init` fails — URL not found | CAF URLs may have moved to CodeLinaro; check https://git.codelinaro.org |
| Missing proprietary blobs | Extract from device or request from Qualcomm |
| Build fails — Jack server OOM | `export JACK_SERVER_VM_ARGUMENTS="-Xmx4g"` |
| `lunch` target not found | Check `device/qcom/sdm660_64/AndroidProducts.mk` exists |
| Ninja build errors | `make clean && make -j$(nproc)` |

---

## Next Steps

- [03_Source_Tree_Layout.md](03_Source_Tree_Layout.md) — Understand the directory structure
- [04_Kernel_Source_Setup.md](04_Kernel_Source_Setup.md) — Kernel-specific build setup
