# HAL Services — Hardware Abstraction Layer

## Overview

**HAL (Hardware Abstraction Layer)** provides a standard interface between Android framework and hardware-specific implementations. SDM660 on Android 9 uses **HIDL (HAL Interface Definition Language)** for Project Treble compliance, separating vendor HALs from the Android framework.

---

## HAL Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Android Framework (Java)                                    │
│  ├── SensorManager                                          │
│  ├── CameraManager                                          │
│  ├── AudioManager                                           │
│  └── ...                                                    │
├──────────────────────────────────────────────────────────────┤
│  HIDL Interface Layer (Binder IPC)                           │
│  ├── android.hardware.sensors@1.0                           │
│  ├── android.hardware.camera.provider@2.4                   │
│  ├── android.hardware.audio@4.0                             │
│  └── android.hardware.graphics.composer@2.1                 │
├──────────────────────────────────────────────────────────────┤
│  Vendor HAL Implementations (/vendor/lib64/hw/)             │
│  ├── sensors.sdm660.so                                      │
│  ├── camera.sdm660.so                                       │
│  ├── audio.primary.sdm660.so                                │
│  └── hwcomposer.sdm660.so                                   │
├──────────────────────────────────────────────────────────────┤
│  Kernel Drivers / Subsystems                                 │
│  ├── IIO / Input (sensors)                                  │
│  ├── V4L2 (camera)                                          │
│  ├── ALSA/ASoC (audio via ADSP)                             │
│  └── DRM/KMS (display)                                      │
└──────────────────────────────────────────────────────────────┘
```

---

## Key HAL Services on SDM660

| HAL | Interface | Service Binary | Purpose |
|-----|-----------|---------------|---------|
| Sensors | sensors@1.0 | android.hardware.sensors@1.0-service | BMI160 accel/gyro |
| Audio | audio@4.0 | android.hardware.audio@4.0-service | Audio via ADSP |
| Camera | camera.provider@2.4 | android.hardware.camera.provider@2.4-service | Camera ISP |
| Display | composer@2.1 | android.hardware.graphics.composer@2.1-service | MDSS/DSI |
| USB | usb@1.0 | android.hardware.usb@1.0-service | DWC3 USB |
| Power | power@1.1 | android.hardware.power@1.1-service | CPU/GPU power hints |
| Thermal | thermal@1.0 | android.hardware.thermal@1.0-service | Thermal management |
| Keymaster | keymaster@3.0 | android.hardware.keymaster@3.0-service-qti | Crypto via TZ |
| Gatekeeper | gatekeeper@1.0 | android.hardware.gatekeeper@1.0-service-qti | Biometric auth via TZ |

---

## Sensor HAL (BMI160)

```
Sensor HAL service startup:
────────────────────────────
1. init parses android.hardware.sensors@1.0-service.rc
2. Starts sensor HAL service process
3. HAL connects to SLPI via QMI (SSC client)
   OR HAL reads from kernel IIO driver (direct mode)
4. Registers with hwservicemanager
5. Framework SensorService discovers HAL
6. Apps can request sensor data

Two architectures:
─────────────────
SLPI mode:  HAL → QMI → SLPI → BMI160 (I2C)
Direct mode: HAL → Linux IIO → BMI160 (I2C, kernel driver)
```

---

## HAL Manifest

```xml
<!-- /vendor/etc/vintf/manifest.xml -->
<manifest version="1.0" type="device">
    <hal format="hidl">
        <name>android.hardware.sensors</name>
        <transport>hwbinder</transport>
        <version>1.0</version>
        <interface>
            <name>ISensors</name>
            <instance>default</instance>
        </interface>
    </hal>
    
    <hal format="hidl">
        <name>android.hardware.audio</name>
        <transport>hwbinder</transport>
        <version>4.0</version>
        <interface>
            <name>IDevicesFactory</name>
            <instance>default</instance>
        </interface>
    </hal>
    <!-- ... more HALs ... -->
</manifest>
```

---

## Debugging HALs

```bash
# List registered HAL services
adb shell lshal

# Check specific HAL
adb shell lshal | grep sensors

# HAL service process
adb shell ps -A | grep "hardware"

# Dump sensor HAL
adb shell dumpsys sensorservice

# HIDL debug
adb shell lshal debug android.hardware.sensors@1.0::ISensors/default
```

---

## Related Documents

- [01_Init_Process.md](01_Init_Process.md) — Init starts HAL services
- [04_Zygote_System_Server.md](04_Zygote_System_Server.md) — Framework connects to HALs
- [../07_Subsystem_Loading/04_SLPI.md](../07_Subsystem_Loading/04_SLPI.md) — SLPI for sensor HAL
