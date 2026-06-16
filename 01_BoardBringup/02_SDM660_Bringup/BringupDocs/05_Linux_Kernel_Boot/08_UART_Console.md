# UART Console — Early Debug Output

## Overview

The **UART (Universal Asynchronous Receiver/Transmitter)** serial console is the primary debug interface during board bring-up. It provides text output from every stage of boot: XBL, ABL, kernel earlycon, full kernel console, and Android init. This document covers the SDM660 UART hardware, configuration, and debug usage.

---

## UART Hardware on SDM660

```
SDM660 UART Architecture:
─────────────────────────
┌─────────────────────────────────────────────────────┐
│  BLSP (BAM Low-Speed Peripheral) QUP              │
│                                                     │
│  BLSP1:                                            │
│  ├── QUP0: I2C1/SPI1                              │
│  ├── QUP1: I2C2/SPI2                              │
│  ├── QUP2: I2C3 (BMI160)                          │
│  ├── QUP3: I2C4/SPI4                              │
│  ├── UART0: ★ Debug UART (ttyMSM0)               │
│  └── UART1: Bluetooth UART                         │
│                                                     │
│  BLSP2:                                            │
│  ├── QUP0-3: Additional I2C/SPI                    │
│  └── UART0-1: Additional UARTs                     │
│                                                     │
│  Debug UART (BLSP1_UART0):                         │
│  ├── Base Address: 0x078AF000                      │
│  ├── IRQ: GIC SPI 107                              │
│  ├── Clock: GCC_BLSP1_UART1_APPS_CLK              │
│  ├── TX Pin: GPIO 4 (function: blsp_uart1)         │
│  ├── RX Pin: GPIO 5 (function: blsp_uart1)         │
│  └── Baud: 115200, 8N1                            │
└─────────────────────────────────────────────────────┘
```

---

## UART Through the Boot Chain

```
┌──────────────────────────────────────────────────────────┐
│  Boot Stage          │ UART Config        │ Output        │
├──────────────────────┼────────────────────┼───────────────┤
│  PBL                 │ Not configured      │ None          │
│  XBL                 │ XBL inits UART      │ [XBL] logs    │
│  ABL/LK              │ Uses XBL config     │ [LK] logs     │
│  Kernel (earlycon)   │ MMIO direct writes  │ Early printk  │
│  Kernel (full)       │ msm_serial driver   │ Full console  │
│  Android (init)      │ /dev/ttyMSM0        │ init logs     │
│  Android (logcat)    │ logcat (not UART)   │ Via adb       │
└──────────────────────────────────────────────────────────┘
```

---

## Earlycon Setup

Earlycon provides debug output before the full serial driver is loaded:

```
Kernel command line (set by ABL):
  earlycon=msm_serial_dm,0x78af000

Flow:
1. start_kernel() → setup_arch() → parse_early_param()
2. earlycon_init(): maps 0x78af000 via fixmap (no full MMU tables yet)
3. register_console(): earlycon becomes active console
4. All printk() output goes to UART via MMIO register writes
5. Later: msm_serial driver probes and takes over
6. earlycon is unregistered (seamless transition)
```

### Earlycon MMIO Register Write

```c
/* Earlycon write function (simplified) */
static void msm_serial_early_putc(struct uart_port *port, int ch)
{
    /* Wait for TX FIFO not full */
    while (readl(port->membase + UART_SR) & UART_SR_TX_FULL)
        ;
    /* Write character to TX FIFO */
    writel(ch, port->membase + UART_TF);
}
```

---

## Device Tree Configuration

```dts
/* UART controller node */
serial@78af000 {
    compatible = "qcom,msm-uartdm-v1.4", "qcom,msm-uartdm";
    reg = <0x78af000 0x200>;
    interrupts = <GIC_SPI 107 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&gcc GCC_BLSP1_UART1_APPS_CLK>,
             <&gcc GCC_BLSP1_AHB_CLK>;
    clock-names = "core", "iface";
    pinctrl-names = "default";
    pinctrl-0 = <&uart_console_active>;
    status = "okay";
};

/* Pin configuration */
&tlmm {
    uart_console_active: uart_console_active {
        mux {
            pins = "gpio4", "gpio5";
            function = "blsp_uart1";
        };
        config {
            pins = "gpio4", "gpio5";
            drive-strength = <2>;
            bias-disable;
        };
    };
};

/* Chosen node for console */
/ {
    chosen {
        stdout-path = "serial0:115200n8";
    };
    aliases {
        serial0 = &blsp1_uart1;
    };
};
```

---

## Hardware Connection

```
SDM660 Board                           Host PC
─────────────────                      ────────
GPIO 4 (TX) ──────── Level Shifter ──── RX (USB-UART adapter)
GPIO 5 (RX) ──────── Level Shifter ──── TX (USB-UART adapter)
GND ──────────────────────────────────── GND

Common USB-UART adapters:
├── FTDI FT232R
├── CP2102/CP2104
├── PL2303
└── CH340G

Host PC settings:
├── Baud rate: 115200
├── Data bits: 8
├── Parity: None
├── Stop bits: 1
├── Flow control: None
└── Software: minicom, picocom, PuTTY, screen
```

### Minicom Setup (Linux Host)

```bash
# Install
sudo apt install minicom

# Configure
sudo minicom -s
# → Serial port setup
#   A - Serial Device: /dev/ttyUSB0
#   E - Baud/Par/Bits: 115200 8N1
#   F - Hardware Flow Control: No

# Run
minicom -D /dev/ttyUSB0 -b 115200

# Capture log to file
minicom -D /dev/ttyUSB0 -b 115200 -C boot_log.txt
```

### PuTTY Setup (Windows Host)

```
Connection type: Serial
Serial line: COM3 (check Device Manager)
Speed: 115200
Data bits: 8
Stop bits: 1
Parity: None
Flow control: None
```

---

## Typical Boot Log (What You See on UART)

```
[XBL] Start
[XBL] PMIC Initialized
[XBL] DDR: 4096 MB, LPDDR4x, Dual Channel
[XBL] DDR Training: PASS
[XBL] TZ Loaded
[XBL] RPM FW Loaded
[XBL] Jumping to ABL...

[LK] Welcome to lk
[LK] platform_init()
[LK] target_init()
[LK] Loading boot image...
[LK] DTB matched: sdm660-mtp (rev 2.0)
[LK] cmdline: console=ttyMSM0,115200 earlycon=msm_serial_dm,0x78af000 ...
[LK] Booting Linux...

[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x51af8014]
[    0.000000] Linux version 4.4.xxx-perf+ (builder@host) ...
[    0.000000] Machine model: Qualcomm Technologies, Inc. SDM660 MTP
[    0.000000] earlycon: msm_serial_dm0 at MMIO 0x078af000
[    0.000000] Command line: console=ttyMSM0,115200n8 ...
[    0.100000] GICv3: CPU0: found redistributor 0 region
[    0.200000] clocksource: arch_sys_counter: mask: 0xffffffffffffff
[    0.300000] Console: colour dummy device 80x25
[    0.400000] msm_serial 78af000.serial: msm_serial: detected port #0
[    0.401000] console [ttyMSM0] enabled
[    0.500000] qcom-gcc 100000.clock-controller: Registered GCC clocks
[    1.000000] i2c_qup 78b7000.i2c: using v2.2.1 tag 36 (0x24)
...
[    5.000000] init: starting Android init...
```

---

## Debugging Tips

| Use Case | Method |
|----------|--------|
| No output at all | Check UART wiring, baud rate, correct GPIO pins |
| XBL output but no kernel | Check earlycon cmdline parameter |
| Kernel panics during boot | Full panic trace visible on UART |
| Intermittent garbled text | Baud rate mismatch or clock config wrong |
| Need to interact | Add `androidboot.console=ttyMSM0` + `adb shell` via serial |

---

## Related Documents

- [01_Early_Assembly_Boot.md](01_Early_Assembly_Boot.md) — Kernel entry before console
- [04_Pinctrl_TLMM.md](04_Pinctrl_TLMM.md) — UART pin configuration
- [03_GCC_Clock_Framework.md](03_GCC_Clock_Framework.md) — UART clock
- [../06_Peripheral_Bringup/03_UART.md](../06_Peripheral_Bringup/03_UART.md) — UART peripheral details
