# PMIC — PM660 / PM660L

## Overview

The SDM660 SoC uses two external **Power Management ICs (PMICs)**: the **PM660** and **PM660L**. These provide all voltage rails, clock generation, GPIO/MPP pins, ADC channels, and power-on/reset logic for the entire platform.

The PMICs connect to the SoC via the **SPMI (System Power Management Interface)** bus — a Qualcomm-designed serial bus optimized for low-latency power management commands.

---

## PMIC Architecture

```
                    ┌──────────────────────────┐
                    │       SDM660 SoC          │
                    │                          │
                    │  ┌──────────────────┐    │
                    │  │  SPMI Master     │    │
                    │  │  Controller      │    │
                    │  └────────┬─────────┘    │
                    │           │ SPMI Bus      │
                    └───────────┼──────────────┘
                                │
                   ┌────────────┴────────────┐
                   │                         │
            ┌──────▼──────┐          ┌──────▼──────┐
            │   PM660     │          │   PM660L    │
            │  (SPMI 0)   │          │  (SPMI 1)   │
            │             │          │             │
            │ • S1-S6     │          │ • S1-S5     │
            │   (Bucks)   │          │   (Bucks)   │
            │ • L1-L19    │          │ • L1-L10    │
            │   (LDOs)    │          │   (LDOs)    │
            │ • BOB       │          │ • BOB       │
            │ • GPIO 1-13 │          │ • GPIO 1-12 │
            │ • MPP 1-4   │          │ • MPP 1-4   │
            │ • RTC       │          │ • WLED      │
            │ • PON       │          │ • Flash LED │
            │ • VADC/ADC  │          │ • LPG (PWM) │
            │ • Charger   │          │             │
            └─────────────┘          └─────────────┘
```

---

## PM660 — Primary PMIC

### Buck Regulators (SMPS)

High-efficiency switch-mode power supplies for major voltage rails:

| Regulator | Voltage | Load | Purpose |
|-----------|---------|------|---------|
| S1 | 0.87 V | High | VDD_CX (Core logic) |
| S2 | 1.05 V | High | VDD_MX (Memory) |
| S3 | 0.6-1.05 V | Very High | VDD_APC (CPU cores, Gold cluster) |
| S4 | 2.04 V | Medium | I/O voltage |
| S5 | 1.35 V | Medium | DDR voltage |
| S6 | 1.128 V | Medium | VDD_APC (CPU cores, Silver cluster) |

### LDO Regulators

Low-dropout regulators for analog and low-noise supplies:

| Regulator | Voltage | Purpose |
|-----------|---------|---------|
| L1 | 1.2 V | MX/CX aux |
| L2 | 1.0 V | GFX (GPU) |
| L3 | 1.0 V | USB SS PHY |
| L6 | 1.2 V | VDDA_CDC (Codec) |
| L8 | 2.95 V | SD card |
| L9 | 1.8 V | I2C pull-up, I/O |
| L11 | 1.8 V | USB PHY |
| L12 | 1.8 V | DSI display |
| L13 | 1.8 V | Sensor VDD (BMI160) |
| L15 | 3.3 V | Sensor VDDIO |
| L17 | 1.8 V | Camera |
| L19 | 3.3 V | Camera analog |

### Power-On Module (PON)

The PON module handles power button events and system reset:

```
Power Button Press (KPDPWR)
    │
    ├── Short Press → Wake from sleep
    ├── Long Press → Power on (if off) or power off menu
    └── Very Long Press (>10s) → Hard reset (PMIC kills all rails, restarts)
```

---

## PM660L — Secondary PMIC

Primarily handles display backlight, flash LED, and additional LDOs:

| Component | Purpose |
|-----------|---------|
| WLED | White LED backlight driver for display |
| Flash LED | Camera flash driver |
| BOB | Boost-Buck regulator (battery to 3.3V) |
| LDOs | Additional low-noise supplies |
| LPG | Light Pulse Generator (PWM for LEDs) |

---

## SPMI Bus

### What is SPMI?

SPMI (System Power Management Interface) is a **2-wire serial bus** (clock + data) designed by Qualcomm/MIPI for PMIC communication. It's similar to I2C but optimized for:

- Very low latency (< 10 μs for a register write)
- Low power (can operate in sleep)
- Multiple slaves on one bus

### SPMI Addressing

```
SPMI Address = SID (Slave ID) + PID (Peripheral ID) + Register Offset

SID 0 = PM660
SID 1 = PM660 (secondary page)
SID 2 = PM660L
SID 3 = PM660L (secondary page)

Example: PM660 GPIO 1 base = SID0:0xC000
         PM660L WLED base  = SID2:0xD800
```

### Device Tree SPMI Configuration

```dts
&spmi_bus {
    qcom,pmic-arb-channel = <0>;

    pm660_0: qcom,pm660@0 {
        compatible = "qcom,spmi-pmic";
        reg = <0x0 SPMI_USID>;
        #address-cells = <2>;
        #size-cells = <0>;

        pm660_pon: qcom,power-on@800 {
            compatible = "qcom,qpnp-power-on";
            reg = <0x800 0x100>;

            /* Power key */
            qcom,pon_1 {
                qcom,pon-type = <0>;  /* KPDPWR */
                qcom,pull-up = <1>;
                linux,code = <116>;   /* KEY_POWER */
            };
        };

        pm660_gpios: pinctrl@c000 {
            compatible = "qcom,spmi-gpio";
            reg = <0xc000 0xd00>;
            gpio-controller;
            #gpio-cells = <2>;
        };

        pm660_vadc: vadc@3100 {
            compatible = "qcom,spmi-vadc";
            reg = <0x3100 0x100>;
            #address-cells = <1>;
            #size-cells = <0>;

            die_temp {
                label = "die_temp";
                reg = <0x6>;
                qcom,decimation = <0>;
                qcom,pre-div-channel-scaling = <0>;
            };
        };
    };

    pm660_regulators: qcom,pm660@1 {
        compatible = "qcom,spmi-pmic";
        reg = <0x1 SPMI_USID>;

        S1: pm660_s1: regulator-s1 {
            regulator-name = "pm660_s1";
            regulator-min-microvolt = <480000>;
            regulator-max-microvolt = <1230000>;
        };

        L13: pm660_l13: regulator-l13 {
            regulator-name = "pm660_l13";
            regulator-min-microvolt = <1780000>;
            regulator-max-microvolt = <1950000>;
            /* Used by BMI160 sensor VDD */
        };
    };
};
```

---

## Voltage Rail Sequencing During Boot

The PMIC powers up rails in a specific sequence — if the order is wrong, the SoC can latch-up or fail to boot.

```
PMIC Power-On Sequence:
───────────────────────
1. Battery → BOB (3.3V) → always-on supply
2. S5 → VDD_DDR (1.35V) → DDR power
3. S1 → VDD_CX (0.87V) → Core logic (must be before CPU)
4. S2 → VDD_MX (1.05V) → Memory retention
5. S3 → VDD_APC Gold (variable) → CPU Gold cluster
6. S6 → VDD_APC Silver (variable) → CPU Silver cluster
7. L-series LDOs → I/O, analog, peripheral power
8. PBL starts executing on CPU0
```

---

## Key PMIC Operations for Bring-Up

### Reading/Writing PMIC Registers

```bash
# From ADB (using SPMI debug interface)
# Read PM660 PON reason
cat /sys/kernel/debug/spmi/spmi-0-00/address   # Set address
echo 0x808 > /sys/kernel/debug/spmi/spmi-0-00/address
cat /sys/kernel/debug/spmi/spmi-0-00/data       # Read value

# From kernel driver
#include <linux/regmap.h>
regmap_read(pmic_regmap, 0x808, &pon_reason);
```

### Regulator Control from Kernel

```c
/* Get regulator handle */
struct regulator *vdd = devm_regulator_get(dev, "vdd");

/* Configure voltage */
regulator_set_voltage(vdd, 1800000, 1800000);  /* 1.8V */

/* Enable rail */
regulator_enable(vdd);

/* Disable rail */
regulator_disable(vdd);
```

---

## PMIC GPIOs vs SoC GPIOs

| Feature | PMIC GPIO (PM660) | SoC GPIO (TLMM) |
|---------|-------------------|------------------|
| Count | 13 (PM660) + 12 (PM660L) | 114 |
| Voltage | PMIC domain (1.8V/3.3V) | SoC I/O domain |
| Access | SPMI (slower) | MMIO (fast) |
| Sleep | Active during PMIC-on sleep | Off during VDD_CX collapse |
| Use cases | Power key, LED control, PMIC IRQs | General I/O, peripheral mux |

---

## Related Documents

- [06_Partition_Layout.md](06_Partition_Layout.md) — Partition layout on storage
- [../01_Power_On_Reset/01_Power_Sequence.md](../01_Power_On_Reset/01_Power_Sequence.md) — PMIC power-up sequence detail
- [../02_XBL_Secondary_Bootloader/04_PMIC_Regulator_Init.md](../02_XBL_Secondary_Bootloader/04_PMIC_Regulator_Init.md) — XBL PMIC init
- [../05_Linux_Kernel_Boot/06_Regulator_Framework.md](../05_Linux_Kernel_Boot/06_Regulator_Framework.md) — Linux regulator framework
