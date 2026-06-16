# SPI — QUP (Qualcomm Universal Peripheral)

## Overview

The same QUP block used for I2C can also operate in **SPI (Serial Peripheral Interface)** mode. SPI is used for high-speed peripherals like display controllers, NOR flash, and sensors that need faster transfer rates than I2C.

---

## SPI on SDM660

```
SPI Bus Map:
────────────
BLSP1:
├── QUP0/SPI1 @ 0x78B5000 (GPIO 0=MOSI, 1=MISO, 2=CLK, 3=CS)
├── QUP1/SPI2 @ 0x78B6000 (GPIO 6-9)
├── QUP2/SPI3 @ 0x78B7000 (GPIO 10-13) [shared with I2C3]
└── QUP3/SPI4 @ 0x78B8000 (GPIO 14-17)

BLSP2:
├── QUP0/SPI5 @ 0xC175000 (GPIO 40-43)
├── QUP1/SPI6 @ 0xC176000 (GPIO 44-47)
├── QUP2/SPI7 @ 0xC177000 (GPIO 48-51)
└── QUP3/SPI8 @ 0xC178000 (GPIO 52-55)

Note: Each QUP is either I2C OR SPI — not both simultaneously.
If QUP2 is used for I2C Bus 3 (BMI160), it cannot also be SPI3.
```

---

## SPI Signals

```
SDM660 (Master)                    SPI Slave Device
─────────────────                  ──────────────────
MOSI (Master Out, Slave In) ─────► DIN / SDI
MISO (Master In, Slave Out) ◄───── DOUT / SDO
SCLK (Clock) ────────────────────► CLK
CS_N (Chip Select, active low) ──► CS / SS
```

---

## Device Tree Example

```dts
spi_1: spi@78b5000 {
    compatible = "qcom,spi-qup-v2.2.1";
    reg = <0x78b5000 0x600>;
    interrupts = <GIC_SPI 95 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&gcc GCC_BLSP1_QUP1_SPI_APPS_CLK>,
             <&gcc GCC_BLSP1_AHB_CLK>;
    clock-names = "core", "iface";
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&spi_1_active>;
    pinctrl-1 = <&spi_1_sleep>;
    spi-max-frequency = <50000000>;  /* 50 MHz max */
    #address-cells = <1>;
    #size-cells = <0>;
    status = "okay";

    /* Example SPI slave device */
    sensor@0 {
        compatible = "vendor,sensor-spi";
        reg = <0>;                         /* CS0 */
        spi-max-frequency = <10000000>;    /* 10 MHz */
        spi-cpol;                          /* CPOL=1 */
        spi-cpha;                          /* CPHA=1 (SPI Mode 3) */
    };
};
```

---

## SPI vs I2C Comparison

| Feature | I2C (QUP) | SPI (QUP) |
|---------|-----------|-----------|
| Pins | 2 (SDA, SCL) | 4 (MOSI, MISO, CLK, CS) |
| Speed | Up to 3.4 MHz | Up to 50 MHz |
| Addressing | 7/10-bit slave address | Chip select line |
| Multi-slave | Shared bus (addresses) | One CS per slave |
| Full duplex | No (half duplex) | Yes |
| Use case | Sensors, PMICs | Display, fast sensors, NOR flash |

---

## Debugging SPI

```bash
# List SPI devices
adb shell ls /dev/spidev*

# Kernel log
adb shell dmesg | grep -i spi

# SPI test with spidev
adb shell spidev_test -D /dev/spidev1.0 -s 1000000 -p "\\x00\\x00"
```

---

## Related Documents

- [01_I2C_QUP.md](01_I2C_QUP.md) — I2C on same QUP block
- [../05_Linux_Kernel_Boot/04_Pinctrl_TLMM.md](../05_Linux_Kernel_Boot/04_Pinctrl_TLMM.md) — SPI pin configuration
