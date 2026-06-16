# BMI160 Sensor Integration on Qualcomm SDM660 — Complete Interview Guide

## Table of Contents

- [1. Project Overview](#1-project-overview)
- [2. Architecture Overview](#2-architecture-overview)
- [3. Hardware Level — Device Tree Configuration](#3-hardware-level--device-tree-configuration)
- [4. Kernel Level — I2C Driver and IIO Subsystem](#4-kernel-level--i2c-driver-and-iio-subsystem)
- [5. DSP Level — SLPI and FastRPC](#5-dsp-level--slpi-and-fastrpc)
- [6. Android Level — Sensor HAL Implementation](#6-android-level--sensor-hal-implementation)
- [7. Complete Data Flow — HAL to Android Application](#7-complete-data-flow--hal-to-android-application)
- [8. Real-Time Data Handling Strategy](#8-real-time-data-handling-strategy)
- [9. Sensor Data Specification](#9-sensor-data-specification)
- [10. Difficulties Faced and Resolutions](#10-difficulties-faced-and-resolutions)
- [11. Testing and Validation](#11-testing-and-validation)
- [12. End-to-End Single Sample Journey](#12-end-to-end-single-sample-journey)
- [13. Common Interview Questions and Answers](#13-common-interview-questions-and-answers)

---

## 1. Project Overview

### Q: What is your project?

I integrated the Bosch BMI160 6-axis IMU sensor on a Qualcomm SDM660 platform running Android 9 Pie. My work spanned the entire stack — from writing the Linux kernel I2C driver and Device Tree bindings, through SLPI offloading via FastRPC, up to the Android Sensor HAL and SensorService integration. The goal was low-latency, low-power, CTS-compliant sensor delivery to Android applications.

The product was an Android device built on Qualcomm's SDM660 chipset. The BMI160 from Bosch is a combined accelerometer + gyroscope sensor connected over I2C.

### My Responsibilities

1. **HARDWARE level** — Configured the Device Tree to describe the sensor's I2C address, interrupt GPIO, and power regulators.
2. **KERNEL level** — Wrote the I2C probe driver that initializes the chip, configures its output data rate, range, reads WHO_AM_I, sets up interrupt-driven data reads, and exposes data via the IIO subsystem.
3. **DSP level** — Integrated with Qualcomm's SLPI (Sensor Low Power Island) so that when the main AP is sleeping, the SLPI continues collecting sensor data into a FIFO, saving power. Communication between AP and SLPI uses FastRPC.
4. **ANDROID level** — Implemented the Sensor HAL (sensors.soc.so) which exposes the accelerometer and gyroscope as separate Android sensors. SensorService picks these up, and apps use SensorManager to consume the data.
5. **TESTING** — Validated using i2cdetect, dmesg, Android sensor test apps, dumpsys sensorservice, and ran CTS/GTS sensor compliance tests.

### Q: What was YOUR specific contribution?
KERNEL DRIVER:
├── Wrote the probe() function that:
│ ├── Verifies chip ID (WHO_AM_I register 0x00 returns 0xD1)
│ ├── Configures accelerometer range (±2g/±4g/±8g/±16g)
│ ├── Configures gyroscope range (±125/±250/±500/±1000/±2000 dps)
│ ├── Sets Output Data Rate (ODR) — e.g., 100Hz, 200Hz, 400Hz
│ ├── Enables data-ready interrupt on INT1 pin
│ └── Registers the IIO device
│
├── Wrote interrupt handler (threaded IRQ) that:
│ ├── Reads 12 bytes (6 for accel + 6 for gyro) in one burst
│ ├── Pushes data to IIO buffer
│ └── Handles FIFO watermark interrupts for batching
│
├── Implemented power management:
│ ├── suspend() — puts sensor in low-power mode
│ └── resume() — restores ODR and range settings

DEVICE TREE:
├── Added bmi160 node under correct I2C bus
├── Configured interrupt GPIO, polarity, pull-up
├── Specified VDD and VDDIO regulators

SLPI / FastRPC:
├── Enabled /dev/fastrpc-sensors device node
├── Configured QMI routing for sensor data path
├── Debugged DSP-side issues using dsps_log

SENSOR HAL:
├── Registered accel and gyro in sensor_list[]
├── Implemented activate(), setDelay(), poll(), batch(), flush()
├── Converted raw ADC values to SI units (m/s², rad/s)
├── Handled batching with FIFO configuration

TESTING:
├── i2cdetect, i2cget/i2cset for bus-level verification
├── dmesg for driver probe logs
├── dumpsys sensorservice for framework registration
├── CTS android.hardware.cts.SensorTest
└── Manual validation with accelerometer test apps



---

## 2. Architecture Overview
┌─────────────────────────────────────────────────────────────┐
│ ANDROID APPLICATION │
│ SensorManager.registerListener() │
├─────────────────────────────────────────────────────────────┤
│ SENSOR SERVICE │
│ (system_server → SensorService.cpp) │
├─────────────────────────────────────────────────────────────┤
│ SENSOR HAL │
│ (sensors.soc.so / sensors.sdm660.so) │
├─────────────────────────────────────────────────────────────┤
│ KERNEL DRIVER │
│ (IIO Subsystem / Input Subsystem) │
├──────────────────────┬──────────────────────────────────────┤
│ MAIN AP PATH │ SLPI PATH │
│ (I2C Driver) │ (Sensor Low Power Island) │
│ │ via FastRPC / QMI │
├──────────────────────┴──────────────────────────────────────┤
│ DEVICE TREE │
│ (I2C bus, GPIO, Regulators, Interrupt) │
├─────────────────────────────────────────────────────────────┤
│ HARDWARE │
│ SDM660 SoC ←──I2C──→ BMI160 Sensor │
│ GPIO IRQ line, VDD/VDDIO power rails │
└─────────────────────────────────────────────────────────────┘



---

## 3. Hardware Level — Device Tree Configuration

### 3.1 BMI160 Sensor Specifications
BMI160 — Bosch Inertial Measurement Unit (IMU)
├── 3-axis Accelerometer (±2g, ±4g, ±8g, ±16g)
├── 3-axis Gyroscope (±125°/s to ±2000°/s)
├── Internal FIFO: 1024 bytes
├── Interface: I2C (up to 1MHz) or SPI (up to 10MHz)
├── I2C Address:
│ ├── 0x68 (SDO pin → GND)
│ └── 0x69 (SDO pin → VDDIO)
├── Interrupt pins: INT1, INT2
├── WHO_AM_I register: 0x00 → returns 0xD1
└── Operating voltage: VDD = 1.71V–3.6V, VDDIO = 1.2V–3.6V



### 3.2 Hardware Schematic

                SDM660 SoC
          ┌─────────────────────┐
          │                     │
          │  I2C_3 (QUP3)      │
          │    SDA ─────────────┼──────── SDA (BMI160 Pin 14)
          │    SCL ─────────────┼──────── SCL (BMI160 Pin 13)
          │                     │
          │  GPIO 23 (IRQ)  ────┼──────── INT1 (BMI160 Pin 12)
          │                     │
          │  PM660L LDO7 ───────┼──1.8V── VDD  (BMI160 Pin 11)
          │  PM660  LDO5 ───────┼──1.8V── VDDIO(BMI160 Pin 1)
          │                     │
          └─────────────────────┘

BMI160 Pin Configuration:
┌──────────────────────┐
│  Pin 1:  VDDIO       │ → I/O level voltage
│  Pin 11: VDD         │ → Core supply
│  Pin 12: INT1        │ → Interrupt output 1
│  Pin 13: SCL         │ → I2C Clock
│  Pin 14: SDA         │ → I2C Data
│  Pin 3:  SDO         │ → I2C address select
│  Pin 5:  GND         │
└──────────────────────┘


### 3.3 I2C Pull-Up Requirement
I2C is an OPEN-DRAIN protocol:

Devices can only pull line LOW

External pull-up resistors pull line HIGH


   VDD (1.8V)
    │
    ├──[4.7kΩ]──── SDA line ────┬──── BMI160 SDA
    │                            │
    ├──[4.7kΩ]──── SCL line ────┬──── BMI160 SCL
    │                            │
   SDM660                       GND (when device pulls low)
Pull-up value depends on:

Bus capacitance (number of devices, trace length)
Bus speed (higher speed → lower resistance → more current)
400kHz Fast Mode: typically 4.7kΩ
1MHz Fast Mode Plus: typically 2kΩ


### 3.4 Device Tree Source

```dts
/* File: arch/arm64/boot/dts/qcom/sdm660-sensor.dtsi */

&i2c_3 {
    status = "okay";
    clock-frequency = <400000>;    /* 400 KHz Fast mode */

    bmi160@68 {
        compatible = "bosch,bmi160";
        reg = <0x68>;

        /* Interrupt configuration */
        interrupt-parent = <&tlmm>;
        interrupts = <23 IRQ_TYPE_EDGE_RISING>;
        interrupt-names = "bmi160_int1";

        /* Power supply */
        vdd-supply = <&pm660l_l7>;     /* 1.8V analog */
        vddio-supply = <&pm660_l5>;    /* 1.8V digital I/O */

        /* Optional: mount matrix for axis remapping */
        mount-matrix = "1", "0", "0",
                       "0", "1", "0",
                       "0", "0", "1";

        /* Pin control */
        pinctrl-names = "default", "sleep";
        pinctrl-0 = <&bmi160_int_active>;
        pinctrl-1 = <&bmi160_int_suspend>;
    };
};

/* GPIO pin configuration */
&tlmm {
    bmi160_int_active: bmi160_int_active {
        mux {
            pins = "gpio23";
            function = "gpio";
        };
        config {
            pins = "gpio23";
            drive-strength = <2>;   /* 2 mA */
            bias-pull-down;
            input-enable;
        };
    };

    bmi160_int_suspend: bmi160_int_suspend {
        mux {
            pins = "gpio23";
            function = "gpio";
        };
        config {
            pins = "gpio23";
            drive-strength = <2>;
            bias-pull-down;
            input-enable;
        };
    };
};
3.5 Mount Matrix Explanation

Physical sensor placement on PCB may differ from Android coordinate system.

Android Coordinate System (phone in portrait, screen facing user):
    Y ↑
      │
      │
      │
      └──────→ X
     /
    / Z (out of screen)

If sensor is rotated 90° on PCB:
mount-matrix = "0", "1", "0",    /* X_android =  Y_sensor */
               "-1", "0", "0",   /* Y_android = -X_sensor */
               "0", "0", "1";    /* Z_android =  Z_sensor */
3.6 DTS Compilation Flow

DTS Source Flow:
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ .dts (board) │───→│     DTC      │───→│  .dtb blob   │
│ + .dtsi      │    │ (compiler)   │    │ (binary)     │
│ (includes)   │    └──────────────┘    └──────┬───────┘
└──────────────┘                               │
                                               ▼
                              ┌─────────────────────────┐
                              │ Bootloader loads .dtb   │
                              │ at fixed memory address  │
                              │ Passes address to kernel │
                              │ via r2 register (ARM)    │
                              └─────────────────────────┘

Qualcomm-specific:
┌──────────┐   ┌──────────────┐   ┌────────────┐
│ sdm660.  │ + │ sdm660-mtp. │ + │ project-   │
│ dtsi     │   │ dtsi         │   │ sensor.dtsi│
│ (SoC)    │   │ (board)      │   │ (custom)   │
└──────────┘   └──────────────┘   └────────────┘
       │              │                  │
       └──────────────┴──────────────────┘
                      │
                      ▼
              Final merged .dtb
3.7 TLMM (Top Level Mode Mux)

Each GPIO on SDM660 can serve multiple functions:
┌─────────┬──────────────────────────────────┐
│ GPIO 22 │ Func 0: GPIO                     │
│         │ Func 1: blsp_i2c6 (SDA)         │
│         │ Func 2: blsp_spi6               │
│         │ Func 3: other                    │
└─────────┴──────────────────────────────────┘

pinctrl subsystem manages:
- Pin muxing (which function)
- Drive strength (2mA, 4mA, 6mA, 8mA, 10mA, 12mA, 14mA, 16mA)
- Bias (pull-up, pull-down, no-pull)
- Input/output enable
- Slew rate
4. Kernel Level — I2C Driver and IIO Subsystem
4.1 I2C Subsystem Architecture

┌─────────────────────────────────────────────────────────┐
│                    USER SPACE                            │
│         /dev/iio:device0  or  /sys/bus/iio/devices/     │
├─────────────────────────────────────────────────────────┤
│                    IIO CORE                              │
│          (Industrial I/O Subsystem)                      │
├─────────────────────────────────────────────────────────┤
│              BMI160 IIO DRIVER                           │
│         (drivers/iio/imu/bmi160/)                        │
├─────────────────────────────────────────────────────────┤
│                I2C CORE                                  │
│         (drivers/i2c/i2c-core.c)                         │
├─────────────────────────────────────────────────────────┤
│            I2C ADAPTER DRIVER                            │
│     (drivers/i2c/busses/i2c-qup.c)                      │
│     QUP = Qualcomm Universal Peripheral                  │
├─────────────────────────────────────────────────────────┤
│                HARDWARE                                  │
│        QUP3 I2C Controller → BMI160                      │
└─────────────────────────────────────────────────────────┘
4.2 Full BMI160 Kernel Driver Implementation

/* File: drivers/iio/imu/bmi160/bmi160_core.c */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>

/* ================================================================
 * BMI160 REGISTER MAP
 * ================================================================ */
#define BMI160_REG_CHIP_ID          0x00
#define BMI160_CHIP_ID_VAL          0xD1

/* Data registers (LSB first, then MSB) */
#define BMI160_REG_GYR_X_LSB       0x0C
#define BMI160_REG_GYR_X_MSB       0x0D
#define BMI160_REG_GYR_Y_LSB       0x0E
#define BMI160_REG_GYR_Y_MSB       0x0F
#define BMI160_REG_GYR_Z_LSB       0x10
#define BMI160_REG_GYR_Z_MSB       0x11
#define BMI160_REG_ACC_X_LSB       0x12
#define BMI160_REG_ACC_X_MSB       0x13
#define BMI160_REG_ACC_Y_LSB       0x14
#define BMI160_REG_ACC_Y_MSB       0x15
#define BMI160_REG_ACC_Z_LSB       0x16
#define BMI160_REG_ACC_Z_MSB       0x17

/* Status register */
#define BMI160_REG_STATUS          0x1B
#define BMI160_STATUS_DRDY_ACC     BIT(7)
#define BMI160_STATUS_DRDY_GYR     BIT(6)

/* Configuration registers */
#define BMI160_REG_ACC_CONF        0x40
#define BMI160_REG_ACC_RANGE       0x41
#define BMI160_REG_GYR_CONF        0x42
#define BMI160_REG_GYR_RANGE       0x43

/* Interrupt registers */
#define BMI160_REG_INT_EN_1        0x51
#define BMI160_REG_INT_MAP_1       0x56
#define BMI160_REG_INT_OUT_CTRL    0x53


