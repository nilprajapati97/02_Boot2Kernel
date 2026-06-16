# UART — Serial Communication

## Overview

In addition to the debug console UART (covered in [../05_Linux_Kernel_Boot/08_UART_Console.md](../05_Linux_Kernel_Boot/08_UART_Console.md)), SDM660 has additional UARTs used for Bluetooth HCI, GPS NMEA, and other serial peripherals. This document covers the UART peripheral bring-up for non-console use cases.

---

## SDM660 UART Map

| UART | BLSP | Base Address | GPIOs | Typical Use |
|------|------|-------------|-------|-------------|
| UART1 | BLSP1 | 0x078AF000 | GPIO 4, 5 | Debug console |
| UART2 | BLSP1 | 0x078B0000 | GPIO 16-19 | Available |
| UART5 | BLSP2 | 0x0C170000 | GPIO 44-47 | Bluetooth HCI |
| UART6 | BLSP2 | 0x0C171000 | GPIO 48-51 | Available |

---

## Bluetooth UART (High-Speed)

The Bluetooth chip (WCN3990/WCN3680B) connects via high-speed UART with hardware flow control:

```dts
/* Bluetooth UART with flow control */
uart_5: serial@c1af000 {
    compatible = "qcom,msm-uartdm-v1.4", "qcom,msm-uartdm";
    reg = <0xc1af000 0x200>;
    interrupts = <GIC_SPI 113 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&gcc GCC_BLSP2_UART2_APPS_CLK>,
             <&gcc GCC_BLSP2_AHB_CLK>;
    clock-names = "core", "iface";
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&uart_bt_active>;    /* TX, RX, CTS, RTS */
    pinctrl-1 = <&uart_bt_sleep>;
    status = "okay";
};

/* 4-wire UART pin config */
&tlmm {
    uart_bt_active: uart_bt_active {
        mux {
            pins = "gpio44", "gpio45", "gpio46", "gpio47";
            function = "blsp_uart5";
        };
        config {
            pins = "gpio44", "gpio45", "gpio46", "gpio47";
            drive-strength = <2>;
            bias-disable;
        };
    };
};
```

---

## UART Configuration

| Parameter | Debug UART | BT UART |
|-----------|-----------|---------|
| Baud rate | 115200 | 3000000 (3 Mbps) |
| Data bits | 8 | 8 |
| Parity | None | None |
| Stop bits | 1 | 1 |
| Flow control | None | Hardware (CTS/RTS) |
| DMA | No (FIFO) | Yes (BAM DMA) |

---

## BAM DMA for High-Speed UART

For Bluetooth UART running at 3 Mbps, FIFO-based transfers would consume too much CPU. Instead, **BAM (Bus Access Manager)** DMA is used:

```
Without DMA (FIFO mode):
  CPU polls UART FIFO → reads 16 bytes → interrupt → repeat
  At 3 Mbps: ~23,000 interrupts/second → CPU waste

With BAM DMA:
  DMA engine reads UART RX → writes to DDR buffer
  Interrupt only when buffer full or idle timeout
  CPU: minimal involvement
```

---

## Debugging UART

```bash
# List serial devices
adb shell ls /dev/ttyMSM*
# /dev/ttyMSM0 (console)
# /dev/ttyMSM1 (BT)

# Test UART loopback
adb shell stty -F /dev/ttyMSM0 115200
adb shell echo "test" > /dev/ttyMSM0

# Check UART driver
adb shell dmesg | grep msm_serial
```

---

## Related Documents

- [../05_Linux_Kernel_Boot/08_UART_Console.md](../05_Linux_Kernel_Boot/08_UART_Console.md) — Debug UART setup
- [01_I2C_QUP.md](01_I2C_QUP.md) — QUP block overview
