# Regulator Framework

## Overview

The Linux **regulator framework** manages voltage and current regulators (PMIC output rails). On SDM660, regulators are controlled via RPM (for shared rails) or directly via SPMI (for processor-local rails). Every peripheral driver requests its power supply through this framework.

---

## Regulator Framework Architecture

```
┌───────────────────────────────────────────────────────────┐
│              Linux Regulator Framework                     │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐  │
│  │  Consumer API (used by peripheral drivers)           │  │
│  │                                                     │  │
│  │  regulator_get()           → Get regulator handle   │  │
│  │  regulator_enable()        → Turn on power          │  │
│  │  regulator_disable()       → Turn off power         │  │
│  │  regulator_set_voltage()   → Set output voltage     │  │
│  │  regulator_get_voltage()   → Read current voltage   │  │
│  │  regulator_set_mode()      → Set operating mode     │  │
│  └────────────────────┬────────────────────────────────┘  │
│                       │                                   │
│  ┌────────────────────▼────────────────────────────────┐  │
│  │  Regulator Core (drivers/regulator/core.c)          │  │
│  │                                                     │  │
│  │  ├── Constraint enforcement (min/max voltage)       │  │
│  │  ├── Reference counting (shared regulators)         │  │
│  │  ├── Supply chain management (parent regulators)    │  │
│  │  └── State machine (enabled/disabled/suspended)     │  │
│  └────────────────────┬────────────────────────────────┘  │
│                       │                                   │
│  ┌────────────────────▼────────────────────────────────┐  │
│  │  Provider Drivers                                    │  │
│  │                                                     │  │
│  │  ┌───────────────────────────────────────────────┐  │  │
│  │  │ RPM SMD Regulator (qcom_smd-regulator.c)     │  │  │
│  │  │ For: VDD_CX, VDD_MX, S1-S6, L1-L19          │  │  │
│  │  │ Path: Driver → RPM message → RPM → SPMI → PMIC│  │  │
│  │  └───────────────────────────────────────────────┘  │  │
│  │                                                     │  │
│  │  ┌───────────────────────────────────────────────┐  │  │
│  │  │ SPMI Regulator (qcom_spmi-regulator.c)       │  │  │
│  │  │ For: Direct PMIC control (non-RPM managed)    │  │  │
│  │  │ Path: Driver → SPMI → PMIC                   │  │  │
│  │  └───────────────────────────────────────────────┘  │  │
│  └─────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────┘
```

---

## How a Driver Gets Power

### Example: BMI160 Sensor Power

```c
/* Sensor driver code */
static int bmi160_probe(struct i2c_client *client)
{
    struct regulator *vdd, *vddio;

    /* Get regulator handles from device tree */
    vdd = devm_regulator_get(&client->dev, "vdd");
    vddio = devm_regulator_get(&client->dev, "vddio");

    /* Set voltage (optional, if not set in DT constraints) */
    regulator_set_voltage(vdd, 1800000, 1800000);  /* 1.8V */
    regulator_set_voltage(vddio, 1800000, 1800000); /* 1.8V */

    /* Enable power */
    regulator_enable(vdd);     /* PM660 L13 turns on */
    regulator_enable(vddio);   /* PM660 L9 turns on */

    /* Sensor can now communicate */
    /* ... I2C read/write ... */

    return 0;
}

static int bmi160_remove(struct i2c_client *client)
{
    regulator_disable(vddio);
    regulator_disable(vdd);
    return 0;
}
```

### Device Tree for BMI160 Power

```dts
bmi160@68 {
    compatible = "bosch,bmi160";
    reg = <0x68>;
    vdd-supply = <&pm660_l13>;     /* 1.8V power supply */
    vddio-supply = <&pm660_l9>;   /* 1.8V I/O supply */
};
```

---

## Regulator DT Definitions

### RPM-Managed Regulators

```dts
&rpm_bus {
    rpm-regulator-ldoa13 {
        status = "okay";
        pm660_l13: regulator-l13 {
            regulator-name = "pm660_l13";
            qcom,set = <3>;              /* Active + Sleep sets */
            regulator-min-microvolt = <1780000>;
            regulator-max-microvolt = <1950000>;
            qcom,init-voltage = <1800000>;
            qcom,init-enable = <0>;       /* Start disabled */
        };
    };
};
```

### Constraint Enforcement

```
regulator_set_voltage(vdd, 2500000, 2500000);  /* Request 2.5V */
→ FAILS! DT constraint: max = 1.95V
→ Returns -EINVAL

regulator_set_voltage(vdd, 1800000, 1800000);  /* Request 1.8V */
→ PASS: Within 1.78V - 1.95V range
→ RPM message sent: key="uv", value=1800000
→ RPM sets PM660 L13 to 1.8V via SPMI
```

---

## Voltage Scaling Flow

```
regulator_set_voltage(vdd, 1800000, 1800000)
    │
    ▼
Regulator Core:
    ├── Check constraints (min/max from DT)
    ├── Check other consumers (if shared regulator)
    │   └── Use MAX of all consumer requests
    └── Call provider ops.set_voltage()
         │
         ▼
    RPM SMD Regulator:
         ├── Build RPM message:
         │   type = "ldoa", id = 13, key = "uv", value = 1800000
         ├── Send via SMD channel to RPM
         └── Wait for ACK
              │
              ▼
         RPM co-processor:
              ├── Aggregate: MAX(all voters for L13)
              ├── Write PMIC register via SPMI:
              │   SID=0, addr=L13_VSET, data=voltage_code
              └── Send ACK
```

---

## Regulator Supply Chains

Some regulators depend on other regulators:

```
PM660 S1 (VDD_CX, 0.8-1.05V, Buck)
    └── Supplies power to SoC core logic
         └── LDOs internally derived from S1

PM660L S3 (1.35V, Buck)
    └── Supplies DDR I/O voltage

PM660 L13 (1.8V, LDO)
    └── Powered from S4 internal rail
         └── Supplies BMI160 VDD
```

---

## Debugging Regulators

```bash
# View all regulators
adb shell cat /sys/kernel/debug/regulator/regulator_summary

# Example output:
# regulator                use  open  bypass  voltage  current  min_uV  max_uV
# ────────────────────────────────────────────────────────────────────────────
# pm660_s1                   1     3       0   872000        0  480000 1230000
# pm660_l13                  1     1       0  1800000        0 1780000 1950000
# pm660_l9                   1     1       0  1800000        0 1750000 1900000

# Check specific regulator
adb shell cat /sys/class/regulator/regulator.X/microvolts
# 1800000

# List consumers
adb shell cat /sys/class/regulator/regulator.X/num_users
# 1

# Force enable (debug only!)
adb shell echo 1 > /sys/class/regulator/regulator.X/state
```

### Common Problems

| Problem | Symptom | Fix |
|---------|---------|-----|
| Regulator not enabled | Peripheral returns 0xFF on I2C | Call regulator_enable() in driver |
| Wrong voltage | Device malfunction | Check DT constraints and init-voltage |
| Supply not defined in DT | -EPROBE_DEFER, driver won't probe | Add `xxx-supply = <&pm660_lXX>` to DT |
| Circular dependency | Boot hang at regulator init | Check supply chain for loops |

---

## Related Documents

- [03_GCC_Clock_Framework.md](03_GCC_Clock_Framework.md) — Clocks + power together
- [../03_RPM_Firmware/02_RPM_Communication.md](../03_RPM_Firmware/02_RPM_Communication.md) — RPM regulator voting
- [../02_XBL_Secondary_Bootloader/04_PMIC_Regulator_Init.md](../02_XBL_Secondary_Bootloader/04_PMIC_Regulator_Init.md) — XBL initial regulator setup
