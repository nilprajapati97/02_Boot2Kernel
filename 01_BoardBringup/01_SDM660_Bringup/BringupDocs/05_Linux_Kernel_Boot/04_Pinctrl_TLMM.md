# Pinctrl — TLMM (Top-Level Mode Multiplexer)

## Overview

The **TLMM (Top-Level Mode Multiplexer)** is SDM660's pin controller. Every GPIO can be configured for multiple functions (I2C, SPI, UART, GPIO, etc.) and the TLMM controls which function is active, along with pull-up/down, drive strength, and direction settings.

---

## TLMM Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    SDM660 TLMM                            │
│                    @ 0x0300_0000                           │
│                                                          │
│  GPIO 0 ─┬─ Function 0: GPIO (general purpose)          │
│           ├─ Function 1: QUP1_SDA (I2C data)            │
│           ├─ Function 2: SPI1_MOSI                       │
│           └─ Function 3: (other)                         │
│                                                          │
│  GPIO 1 ─┬─ Function 0: GPIO                            │
│           ├─ Function 1: QUP1_SCL (I2C clock)           │
│           ├─ Function 2: SPI1_MISO                       │
│           └─ Function 3: (other)                         │
│                                                          │
│  ... 113 GPIOs total (GPIO 0 - GPIO 113) ...             │
│                                                          │
│  Each GPIO has:                                          │
│  ├── CFG register (function select, pull, drive)         │
│  ├── IN/OUT register (value)                             │
│  └── INTR register (interrupt config)                    │
└──────────────────────────────────────────────────────────┘
```

---

## GPIO Register Layout

Each GPIO has a set of registers at offset `0x1000 * gpio_num`:

```
GPIO N registers (base = 0x03000000 + N * 0x1000):
────────────────────────────────────────────────────
Offset 0x00: GPIO_CFG
  Bits [1:0]   PULL:     00=none, 01=pull-down, 10=keeper, 11=pull-up
  Bits [5:2]   FUNC_SEL: 0=GPIO, 1-15=alternate function
  Bits [8:6]   DRV_STR:  000=2mA, 001=4mA, 010=6mA, 011=8mA,
                          100=10mA, 101=12mA, 110=14mA, 111=16mA
  Bit  [9]     OE:       0=input, 1=output enable

Offset 0x04: GPIO_IN_OUT
  Bit [0]  GPIO_IN:   Read input value
  Bit [1]  GPIO_OUT:  Write output value

Offset 0x08: GPIO_INTR_CFG
  Bit [0]  INTR_ENABLE:    0=disabled, 1=enabled
  Bit [1]  INTR_POL:       0=active low, 1=active high
  Bit [2]  INTR_DETECT_CTL: 00=level, 01=rising, 10=falling, 11=both
  Bit [4]  INTR_RAW_STATUS: Read raw interrupt status

Offset 0x0C: GPIO_INTR_STATUS
  Bit [0]  INTR_STATUS:   Write 1 to clear
```

---

## Pin Configurations in Device Tree

### Pinctrl Provider Node

```dts
tlmm: pinctrl@3000000 {
    compatible = "qcom,sdm660-pinctrl";
    reg = <0x03000000 0xc00000>;
    interrupts = <GIC_SPI 208 IRQ_TYPE_LEVEL_HIGH>;
    gpio-controller;
    #gpio-cells = <2>;
    interrupt-controller;
    #interrupt-cells = <2>;
};
```

### Pin Group Definitions (I2C Bus 3 — for BMI160)

```dts
&tlmm {
    i2c_3_active: i2c_3_active {
        mux {
            pins = "gpio10", "gpio11";
            function = "blsp_i2c3";    /* Alternate function */
        };
        config {
            pins = "gpio10", "gpio11";
            drive-strength = <2>;       /* 2 mA */
            bias-disable;               /* No pull (I2C has external pull-ups) */
        };
    };

    i2c_3_sleep: i2c_3_sleep {
        mux {
            pins = "gpio10", "gpio11";
            function = "gpio";          /* GPIO mode when sleeping */
        };
        config {
            pins = "gpio10", "gpio11";
            drive-strength = <2>;
            bias-pull-down;             /* Pull down to save power */
        };
    };

    /* BMI160 interrupt pin */
    bmi160_int_active: bmi160_int_active {
        mux {
            pins = "gpio23";
            function = "gpio";          /* Used as GPIO interrupt */
        };
        config {
            pins = "gpio23";
            drive-strength = <2>;
            bias-pull-down;             /* Pull-down, sensor drives high */
            input-enable;               /* Configure as input */
        };
    };
};
```

### Consumer: I2C Controller Uses Pinctrl

```dts
i2c_3: i2c@78b7000 {
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&i2c_3_active>;        /* Applied when device active */
    pinctrl-1 = <&i2c_3_sleep>;         /* Applied during suspend */
    status = "okay";
};
```

---

## Pinctrl State Machine

```
Device Driver Lifecycle vs Pin States:
──────────────────────────────────────

Driver probe()
    │
    ├── pinctrl_select_state("default")  → i2c_3_active applied
    │   GPIO 10: func=blsp_i2c3, pull=none, drive=2mA
    │   GPIO 11: func=blsp_i2c3, pull=none, drive=2mA
    │
    ▼
Device in use (I2C transfers happening)
    │
    ├── System suspend
    │   └── pinctrl_select_state("sleep")  → i2c_3_sleep applied
    │       GPIO 10: func=gpio, pull=down, drive=2mA
    │       GPIO 11: func=gpio, pull=down, drive=2mA
    │       (saves power: no floating pins)
    │
    ├── System resume
    │   └── pinctrl_select_state("default")  → i2c_3_active restored
    │
    ▼
Driver remove()
    └── Pins released
```

---

## Common SDM660 Pin Functions

| GPIO | Function 0 (GPIO) | Function 1 | Function 2 | Common Use |
|------|-------------------|------------|------------|------------|
| 0-1 | GPIO | BLSP1_QUP1_SDA/SCL | - | I2C Bus 1 |
| 6-7 | GPIO | BLSP1_QUP2_SDA/SCL | - | I2C Bus 2 |
| 10-11 | GPIO | BLSP1_QUP3_SDA/SCL | - | **I2C Bus 3 (BMI160)** |
| 0-3 | GPIO | BLSP1_SPI1 | - | SPI Bus 1 |
| 4-5 | GPIO | BLSP1_UART1 TX/RX | - | Debug UART |
| 23 | **GPIO (INT)** | - | - | **BMI160 interrupt** |
| 40-43 | GPIO | BLSP2_QUP1 | - | I2C Bus 5 |
| 81-82 | GPIO | USB_PHY | - | USB data |

---

## Debugging Pinctrl

```bash
# View all pin groups
adb shell cat /sys/kernel/debug/pinctrl/3000000.pinctrl/pingroups

# View current pin function
adb shell cat /sys/kernel/debug/pinctrl/3000000.pinctrl/pins

# View pin mux setting
adb shell cat /sys/kernel/debug/pinctrl/3000000.pinctrl/pinmux-pins

# GPIO status
adb shell cat /sys/kernel/debug/gpio

# Example output:
# GPIOs 0-113, platform/3000000.pinctrl, 3000000.pinctrl:
#  gpio-10  (i2c_3_sda           ) in  lo
#  gpio-11  (i2c_3_scl           ) in  lo
#  gpio-23  (bmi160_int           ) in  lo IRQ
```

### Common Problems

| Problem | Symptom | Fix |
|---------|---------|-----|
| Wrong function selected | I2C/SPI not working | Check function name in DT matches TLMM spec |
| Missing pull-up | I2C bus stuck | Add external pull-ups or enable bias-pull-up |
| Wrong drive strength | Signal integrity issues | Increase drive-strength (4mA, 6mA) |
| Pin conflict | Two drivers claim same GPIO | Check for overlapping pin assignments in DT |
| Sleep state not set | High power in suspend | Define and apply sleep pinctrl state |

---

## Related Documents

- [02_Device_Tree_Processing.md](02_Device_Tree_Processing.md) — DT pinctrl bindings
- [05_GIC_Interrupt_Controller.md](05_GIC_Interrupt_Controller.md) — GPIO interrupts via GIC
- [../06_Peripheral_Bringup/01_I2C_QUP.md](../06_Peripheral_Bringup/01_I2C_QUP.md) — I2C uses pinctrl for SDA/SCL
