Configuring the **PMIC**, **clock tree**, and **pinmux** is a key part of bringing up an embedded Linux board. These components are initialized early—either in **bootloader (SPL/U-Boot)** or **Device Tree (DT)**—and affect **power, timing, and I/O** of the entire SoC.

---

# ⚙️ Full Guide: Configure PMIC, Clock Tree, and Pinmux on Embedded Linux Board

---

## 1. 🔋 PMIC (Power Management IC) Configuration

### 📌 Purpose:

* Supplies regulated voltages to CPU, DDR, peripherals
* Handles **power sequencing**, voltage scaling (DVFS), sleep, reset
* Communicates over **I2C/SMBus**

---

### 🔧 Where It’s Configured?

| Stage            | How it’s Configured                   |
| ---------------- | ------------------------------------- |
| **SPL / U-Boot** | Via I2C in board init code / driver   |
| **Device Tree**  | Describe PMIC node, regulators        |
| **Linux Kernel** | Regulator framework (`regulator_ops`) |

---

### 📄 Device Tree Example for PMIC (TI TPS65910)

```dts
pmic: tps65910@2d {
    compatible = "ti,tps65910";
    reg = <0x2d>;  // I2C address
    regulators {
        vdd1_reg: vdd1 {
            regulator-name = "vdd_core";
            regulator-min-microvolt = <900000>;
            regulator-max-microvolt = <1300000>;
            regulator-always-on;
        };
        vdd2_reg: vdd2 {
            regulator-name = "vdd_mpu";
            regulator-min-microvolt = <900000>;
            regulator-max-microvolt = <1200000>;
        };
    };
};
```

---

### 🔌 In U-Boot

```c
struct udevice *dev;
i2c_get_chip_for_busnum(0, 0x2d, 1, &dev);
i2c_write(dev, reg_addr, 1, &val, 1);  // configure LDOs or DCDC
```

---

## 2. ⏱️ Clock Tree Configuration

### 📌 Purpose:

* Controls **PLL**, **clock source**, **dividers**
* Enables/disables clocks to **peripherals**
* Configured by **bootloader**, enforced in **Device Tree**

---

### 🔧 Where It’s Configured?

| Component         | Configured In            | Description                 |
| ----------------- | ------------------------ | --------------------------- |
| PLL setup         | U-Boot SPL or ROM        | Initialize core/system PLLs |
| Peripheral clocks | Device Tree              | Clock tree nodes for Linux  |
| Dynamic changes   | Kernel drivers / cpufreq | DVFS, suspend/resume        |

---

### 📄 Device Tree Clock Example (i.MX6)

```dts
clocks {
    #address-cells = <1>;
    #size-cells = <0>;

    uart1_clk: uart1_clk {
        compatible = "fixed-clock";
        #clock-cells = <0>;
        clock-frequency = <80000000>;
    };
};

&uart1 {
    clocks = <&uart1_clk>;
    clock-names = "per";
    status = "okay";
};
```

---

### 🔌 In U-Boot SPL (i.MX/NXP Example)

```c
enable_uart_clk(1);    // enable PLL, gate, mux for UART
setup_core_pll();      // configure PLL for 1GHz
```

---

### 📐 Tools to View Clock Tree in Linux

```bash
cat /sys/kernel/debug/clk/clk_summary
```

This shows current frequencies, parents, and enable status.

---

## 3. 🧲 Pinmux / IO Multiplexing Configuration

### 📌 Purpose:

* Maps SoC pins to **desired function** (UART, I2C, SPI, GPIO)
* Configures **pull-up/down, drive strength, slew rate**

---

### 🔧 Where It’s Configured?

| Stage       | Tool                        | Action                          |
| ----------- | --------------------------- | ------------------------------- |
| U-Boot SPL  | `mux.c` or `board.c`        | Configure pinctrl registers     |
| Device Tree | `pinctrl-*` nodes           | Describe groups and configs     |
| Kernel      | `pinctrl` subsystem applies | Applies based on driver binding |

---

### 📄 Device Tree Pinmux Example (AM335x)

```dts
pinctrl_uart1: pinmux_uart1_pins {
    pinctrl-single,pins = <
        0x180 (PIN_INPUT_PULLUP | MUX_MODE0)  // uart1_rxd
        0x184 (PIN_OUTPUT | MUX_MODE0)        // uart1_txd
    >;
};

&uart1 {
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_uart1>;
    status = "okay";
};
```

* `PIN_INPUT_PULLUP`, `MUX_MODE0` are macros from SoC header.

---

### 📌 U-Boot SPL Pinmux (TI AM335x Example)

```c
static struct module_pin_mux uart1_pin_mux[] = {
    {OFFSET(uart1_rxd), (MODE(0) | PULLUP_EN | RXACTIVE)},
    {OFFSET(uart1_txd), (MODE(0) | PULLUDEN)},
    {-1},
};

configure_module_pin_mux(uart1_pin_mux);
```

---

### 🧪 View Active Pin Config in Linux

```bash
cat /sys/kernel/debug/pinctrl/<soc>-pinctrl/ | less
```

---

## ✅ Summary Table

| Component | Config in DT | Config in U-Boot | Kernel Support        | Tools to Verify                     |
| --------- | ------------ | ---------------- | --------------------- | ----------------------------------- |
| PMIC      | Yes          | Optional (I2C)   | `regulator` framework | `/sys/class/regulator`              |
| Clocks    | Yes          | Yes (early PLL)  | `clk` framework       | `/sys/kernel/debug/clk/clk_summary` |
| Pinmux    | Yes          | Optional         | `pinctrl` framework   | `/sys/kernel/debug/pinctrl`         |

---

## 📦 Final Checklist for Board Bring-Up

| Task                          | Status |
| ----------------------------- | ------ |
| PMIC I2C accessible           | ✅      |
| Regulators defined in DT      | ✅      |
| SPL initializes PLLs          | ✅      |
| Clock tree defined in DT      | ✅      |
| UART/I2C/SPI pinmux applied   | ✅      |
| Boot logs show clean power-up | ✅      |

---

Would you like:

* A live DTS example for your SoC/board?
* Register-level I2C PMIC config example?
* U-Boot pinmux setup template for a specific pin?

Let me know your target platform (e.g. AM335x, i.MX6, STM32MP1, etc.) and I’ll tailor it!
