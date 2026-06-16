# I2C — QUP (Qualcomm Universal Peripheral)

## Overview

The **QUP (Qualcomm Universal Peripheral)** is a versatile serial controller that can operate in I2C, SPI, or UART mode. On SDM660, I2C buses are implemented using QUP controllers within the **BLSP (BAM Low-Speed Peripheral)** blocks. This document covers I2C mode bring-up, focusing on Bus 3 used for BMI160 sensor.

---

## SDM660 I2C Bus Map

```
┌──────────────────────────────────────────────────────────────┐
│  BLSP1 (BAM Low-Speed Peripheral Block 1)                    │
│  Base: 0x0788_9000 region                                    │
│                                                              │
│  ├── QUP0: I2C Bus 1 @ 0x78B5000 (GPIO 0, 1)              │
│  ├── QUP1: I2C Bus 2 @ 0x78B6000 (GPIO 6, 7)              │
│  ├── QUP2: I2C Bus 3 @ 0x78B7000 (GPIO 10, 11) ★ BMI160   │
│  └── QUP3: I2C Bus 4 @ 0x78B8000 (GPIO 14, 15)            │
│                                                              │
│  BLSP2 (BAM Low-Speed Peripheral Block 2)                    │
│  Base: 0x0C17_5000 region                                    │
│                                                              │
│  ├── QUP0: I2C Bus 5 @ 0xC175000 (GPIO 40, 41)            │
│  ├── QUP1: I2C Bus 6 @ 0xC176000 (GPIO 44, 45)            │
│  ├── QUP2: I2C Bus 7 @ 0xC177000 (GPIO 48, 49)            │
│  └── QUP3: I2C Bus 8 @ 0xC178000 (GPIO 52, 53)            │
└──────────────────────────────────────────────────────────────┘
```

---

## I2C Bus 3 Configuration (BMI160)

```
I2C Bus 3 → BMI160 Connection:
──────────────────────────────
                                        ┌──────────┐
SDM660                                  │ BMI160   │
  GPIO 10 (SDA) ──── Pull-up ──────── │ SDA      │
  GPIO 11 (SCL) ──── Pull-up ──────── │ SCL      │
  GPIO 23 (INT) ◄────────────────────── │ INT1     │
  PM660 L13 ────── 1.8V ──────────────│ VDD      │
  PM660 L9  ────── 1.8V ──────────────│ VDDIO    │
                                        │          │
                                        │ Addr:0x68│
                                        │ (SDO=GND)│
                                        └──────────┘

Pull-up: 4.7 KΩ to VDDIO (1.8V)
Speed: 400 KHz (Fast Mode)
```

---

## I2C Driver Probe Sequence

```
Kernel boot:
1. GCC driver probes → registers GCC_BLSP1_QUP3_I2C_APPS_CLK
2. Pinctrl (TLMM) driver probes → registers i2c_3_active/sleep pin groups
3. RPM regulator driver probes → registers pm660_l13, pm660_l9

I2C QUP driver probe (drivers/i2c/busses/i2c-qup.c):
4. of_match: compatible = "qcom,i2c-qup-v2.2.1"
5. Get resources from DT:
   ├── reg = <0x78b7000 0x600>    → ioremap MMIO registers
   ├── interrupts = <GIC_SPI 97>  → request_irq for transfer completion
   ├── clocks: core + iface       → clk_prepare_enable()
   └── pinctrl: default           → select i2c_3_active pin state

6. Configure QUP:
   ├── Set QUP to I2C master mode
   ├── Set clock divider for 400 KHz
   │   └── Source = 50 MHz → divider = 50M / (2 × 400K) = 62
   ├── Configure FIFO thresholds
   └── Enable QUP core

7. Register I2C adapter with i2c_add_adapter()
   └── Creates /dev/i2c-3

8. of_i2c_register_devices() scans DT children:
   └── Finds bmi160@68 → creates i2c_client → triggers bmi160 probe
```

---

## QUP Register Map

```
QUP I2C Registers (base = 0x78B7000):
──────────────────────────────────────
Offset  Register              Purpose
0x000   QUP_CONFIG            QUP mode config (I2C/SPI/UART)
0x004   QUP_STATE             State machine control (RUN/PAUSE/RESET)
0x008   QUP_IO_MODES          FIFO/Block mode selection
0x018   QUP_OPERATIONAL       Current operational status
0x100   QUP_OUTPUT_FIFO       TX FIFO (write data here)
0x108   QUP_INPUT_FIFO        RX FIFO (read data from here)
0x200   QUP_MX_OUTPUT_CNT     Max output byte count
0x208   QUP_MX_INPUT_CNT      Max input byte count
0x400   I2C_MASTER_CLK_CTL    I2C clock divider
0x408   I2C_MASTER_STATUS     I2C status (ACK/NACK/errors)
```

---

## I2C Transfer Flow

```
I2C Write (e.g., write BMI160 register):
─────────────────────────────────────────
i2c_smbus_write_byte_data(client, 0x7E, 0xB6) // CMD_REG=0x7E, data=0xB6

1. i2c-qup driver: qup_i2c_xfer() called
2. Set QUP state to RUN
3. Write to output FIFO:
   ├── Start condition + slave addr (0x68 << 1 | 0 = 0xD0) [write]
   ├── Register address: 0x7E
   └── Data byte: 0xB6
4. QUP generates I2C waveform:
   ├── START → 0xD0 → ACK → 0x7E → ACK → 0xB6 → ACK → STOP
5. Interrupt fires (SPI 97) → completion signaled
6. Check I2C_MASTER_STATUS for errors (NACK, bus error)

I2C Read (e.g., read BMI160 chip ID):
──────────────────────────────────────
i2c_smbus_read_byte_data(client, 0x00) // CHIP_ID register

1. Write phase:
   ├── START → 0xD0 (write) → ACK → 0x00 (register) → ACK
2. Read phase:
   ├── RESTART → 0xD1 (read) → ACK → [data] → NACK → STOP
3. Read input FIFO → data byte (0xD1 = BMI160 chip ID)
```

---

## Device Tree Configuration

```dts
i2c_3: i2c@78b7000 {
    compatible = "qcom,i2c-qup-v2.2.1";
    reg = <0x78b7000 0x600>;
    reg-names = "qup_phys_addr";
    interrupts = <GIC_SPI 97 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&gcc GCC_BLSP1_QUP3_I2C_APPS_CLK>,
             <&gcc GCC_BLSP1_AHB_CLK>;
    clock-names = "core", "iface";
    clock-frequency = <400000>;
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&i2c_3_active>;
    pinctrl-1 = <&i2c_3_sleep>;
    qcom,noise-rjct-scl = <0>;
    qcom,noise-rjct-sda = <0>;
    status = "okay";

    bmi160@68 {
        compatible = "bosch,bmi160";
        reg = <0x68>;
        interrupt-parent = <&tlmm>;
        interrupts = <23 IRQ_TYPE_EDGE_RISING>;
        vdd-supply = <&pm660_l13>;
        vddio-supply = <&pm660_l9>;
    };
};
```

---

## Debugging I2C

```bash
# List I2C buses
adb shell ls /dev/i2c-*
# /dev/i2c-3

# Scan bus for devices (requires i2c-tools)
adb shell i2cdetect -y 3
#      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
# 60: -- -- -- -- -- -- -- -- 68 -- -- -- -- -- -- --
#                                ^^
#                           BMI160 responds at 0x68

# Read chip ID register
adb shell i2cget -y 3 0x68 0x00
# 0xd1 (BMI160 chip ID)

# Dump registers
adb shell i2cdump -y 3 0x68

# Kernel log
adb shell dmesg | grep -i "i2c\|qup\|bmi160"
# [    1.000] i2c_qup 78b7000.i2c: using v2.2.1 tag 36
# [    1.200] bmi160 3-0068: chip id: 0xd1
```

### Common I2C Problems

| Problem | Symptom | Fix |
|---------|---------|-----|
| NACK on address | i2cdetect shows nothing | Check slave address, pull-ups, power supply |
| Bus stuck (SDA low) | All transfers timeout | Toggle SCL 9 times to unstick, check for shorts |
| Clock too fast | Intermittent NACK | Reduce clock-frequency in DT |
| Wrong function select | No waveform on scope | Check TLMM pin function = "blsp_i2c3" |
| Clock not enabled | I2C reads return 0 | Check GCC_BLSP1_QUP3_I2C_APPS_CLK is on |

---

## Related Documents

- [../05_Linux_Kernel_Boot/04_Pinctrl_TLMM.md](../05_Linux_Kernel_Boot/04_Pinctrl_TLMM.md) — I2C pin config
- [../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md](../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md) — I2C clock
- [../05_Linux_Kernel_Boot/06_Regulator_Framework.md](../05_Linux_Kernel_Boot/06_Regulator_Framework.md) — Sensor power
- [02_SPI_QUP.md](02_SPI_QUP.md) — SPI on same QUP block
