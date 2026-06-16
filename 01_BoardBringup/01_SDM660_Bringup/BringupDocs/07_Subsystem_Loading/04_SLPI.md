# SLPI — Sensor Low-Power Island

## Overview

The **SLPI (Sensor Low-Power Island)** is a dedicated Hexagon processor for sensor processing. It runs the **SNS (Sensor) framework** and handles accelerometer, gyroscope, magnetometer, proximity, light, and other sensor data. **The BMI160 IMU sensor data is processed on the SLPI** when using Qualcomm's Sensor Hub architecture.

---

## SLPI Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                                                              │
│  Apps Processor                                             │
│  ├── Android Sensor HAL                                     │
│  ├── SSC (Sensor Service Client) ──┐                       │
│  └── QMI client                    │ QMI/GLINK              │
│                                    │                        │
│  ┌─────────────────────────────────▼──────────────────────┐ │
│  │  SLPI (Hexagon, Low-Power Island)                      │ │
│  │                                                        │ │
│  │  ┌──────────────────────────────────────────────────┐  │ │
│  │  │  SNS Framework (Qualcomm Sensor Hub)             │  │ │
│  │  │                                                  │  │ │
│  │  │  ┌──────────┐ ┌──────────┐ ┌──────────┐        │  │ │
│  │  │  │ BMI160   │ │ ALS/Prox │ │ Mag      │        │  │ │
│  │  │  │ Driver   │ │ Driver   │ │ Driver   │  ...   │  │ │
│  │  │  │ (Accel + │ │          │ │          │        │  │ │
│  │  │  │  Gyro)   │ │          │ │          │        │  │ │
│  │  │  └────┬─────┘ └──────────┘ └──────────┘        │  │ │
│  │  │       │                                         │  │ │
│  │  │  ┌────▼───────────────────────────────────┐     │  │ │
│  │  │  │  Sensor Fusion / Algorithms            │     │  │ │
│  │  │  │  ├── Gravity estimation                │     │  │ │
│  │  │  │  ├── Step counter / pedometer          │     │  │ │
│  │  │  │  ├── Orientation                       │     │  │ │
│  │  │  │  └── Activity recognition              │     │  │ │
│  │  │  └────────────────────────────────────────┘     │  │ │
│  │  └──────────────────────────────────────────────────┘  │ │
│  │                                                        │ │
│  │  Low-Power: SLPI can run while Apps CPU sleeps         │ │
│  │  Power: ~1-5 mW (vs ~100+ mW for Apps CPU)            │ │
│  └────────────────────────────────────────────────────────┘ │
│                        │ I2C/SPI                            │
│                   ┌────▼─────┐                              │
│                   │ BMI160   │                              │
│                   │ Sensor   │                              │
│                   │ I2C:0x68 │                              │
│                   └──────────┘                              │
└──────────────────────────────────────────────────────────────┘
```

---

## Two Sensor Architectures

### Architecture 1: Direct Kernel Driver (AP-side)

```
BMI160 → I2C Bus 3 → Apps CPU → Linux IIO driver → Android HAL
  Pros: Simple, standard Linux
  Cons: CPU must be awake for sensor data, higher power
```

### Architecture 2: SLPI Sensor Hub (Qualcomm preferred)

```
BMI160 → I2C Bus 3 → SLPI Hexagon → SNS Framework →
  QMI → Apps CPU → Android Sensor HAL
  Pros: Low power (SLPI runs while AP sleeps), sensor fusion
  Cons: More complex, proprietary framework
```

---

## SLPI Loading

```
PIL loads SLPI firmware:
1. request_firmware("slpi.mdt") from /lib/firmware/
2. Load segments to reserved DDR @ 0x94e00000 (20 MB)
3. TZ authenticates and releases SLPI from reset
4. SLPI boots QuRT RTOS + SNS framework
5. SLPI discovers connected sensors via I2C probing
6. QMI sensor service registers with Apps processor
7. Android Sensor HAL connects via QMI

Kernel log:
[    4.000] subsys-pil-tz 15200000.qcom,slpi: firmware: requesting slpi.mdt
[    4.300] subsys-pil-tz 15200000.qcom,slpi: slpi: Brought up successfully
[    4.500] sns_qmi: Sensor service connected
```

---

## BMI160 on SLPI

When using the SLPI architecture, the BMI160 I2C bus is connected to the SLPI's I2C controller (shared GPIO mux):

```
SLPI Sensor Discovery:
1. SLPI boots → reads sensor registry (JSON config)
2. Registry specifies: BMI160 on I2C bus 3, addr 0x68
3. SLPI BMI160 driver probes → reads chip ID → 0xD1
4. SLPI configures BMI160:
   ├── Accel: 100 Hz, ±8g range
   ├── Gyro: 100 Hz, ±2000 dps range
   └── Interrupt: data ready on INT1 (GPIO 23)
5. SLPI registers sensor types with SNS framework:
   ├── SENSOR_TYPE_ACCELEROMETER
   ├── SENSOR_TYPE_GYROSCOPE
   └── SENSOR_TYPE_STEP_COUNTER (derived)
```

---

## Device Tree

```dts
slpi_pil: qcom,msm-slpi-loader {
    compatible = "qcom,slpi-pil-tz";
    qcom,firmware-name = "slpi";
    memory-region = <&slpi_fw_mem>;
    qcom,ssctl-instance-id = <0x16>;
    
    /* I2C bus allocation to SLPI */
    qcom,gpio-force-stop = <&smp2pgpio_sleepstate_SLPI 0 0>;
};
```

---

## SLPI Sensor Registry

The sensor registry configures which sensors are connected:

```json
{
  "config": {
    "hw_platform": "MTP",
    "soc_id": "317"
  },
  "accel": {
    "driver": "bmi160",
    "bus_type": "I2C",
    "bus_instance": 3,
    "slave_addr": "0x68",
    "interrupt_gpio": 23,
    "placement": {
      "x_axis": 1,
      "y_axis": 2,
      "z_axis": 3
    }
  },
  "gyro": {
    "driver": "bmi160",
    "bus_type": "I2C",
    "bus_instance": 3,
    "slave_addr": "0x68"
  }
}
```

---

## Debugging SLPI

```bash
# Check SLPI status
adb shell cat /sys/bus/msm_subsys/devices/subsys2/state
# ONLINE

# Sensor list (via Android)
adb shell dumpsys sensorservice | head -40

# SNS debug log
adb shell cat /sys/kernel/debug/slpi/sns_log

# SLPI crash dump
adb shell ls /data/vendor/ssrdump/
```

---

## Related Documents

- [01_PIL_Framework.md](01_PIL_Framework.md) — Firmware loading
- [../06_Peripheral_Bringup/01_I2C_QUP.md](../06_Peripheral_Bringup/01_I2C_QUP.md) — I2C bus for BMI160
- [../08_IPC_Mechanisms/05_QMI.md](../08_IPC_Mechanisms/05_QMI.md) — QMI sensor service
