Adding support for a **new UART peripheral** in your **SoC (System on Chip)** for an **Embedded Linux BSP** involves both **platform-level integration** and **driver configuration**. Below is a step-by-step, **in-depth guide** covering all layers from **device tree** to **driver binding**, testing, and debugging.

---

## 🧩 1. Overview: What is UART Support?

Adding UART support typically means:

* Declaring the UART in the **Device Tree**.
* Ensuring proper **clock, pinmux, and reset** configurations.
* Making sure the appropriate **UART driver** is selected and configured in the **kernel**.
* Verifying **register mappings** and **interrupt lines**.
* Debugging using console, `dmesg`, and `minicom` or `picocom`.

---

## ⚙️ 2. Add UART in Device Tree

> Let’s assume your UART is memory-mapped and behaves like a standard 16550 UART.

### 📄 Device Tree Snippet:

```dts
uart3: serial@78b0000 {
    compatible = "snps,dw-apb-uart";   // Based on your IP
    reg = <0x078b0000 0x1000>;         // Base address and size
    interrupts = <0 123 4>;            // GIC interrupt
    clock-frequency = <48000000>;      // Optional
    clocks = <&uart_clk>;
    pinctrl-names = "default";
    pinctrl-0 = <&uart3_pins>;
    status = "okay";
};
```

📌 **Notes**:

* `reg`: Memory region for UART registers.
* `interrupts`: IRQ from SoC-specific IRQ controller.
* `clocks`: You may also need to enable and define this clock node in the clock controller.
* `pinctrl`: Optional if pinmuxing is required.

---

## 🔧 3. Enable UART Driver in Kernel

### 📁 `make menuconfig`

Navigate to:

```bash
Device Drivers → Character devices → Serial drivers
```

✔ Enable your driver. For DesignWare:

```bash
[*] Serial driver for 8250/16550 and compatible UARTs
[*]   Support for 8250/16550-like UARTs
[*]   Support for DesignWare APB UART
```

> Add support for your driver if it’s vendor-specific (e.g., `CONFIG_SERIAL_QCOMGENI` for Qualcomm Geni UARTs).

---

## 🧠 4. Verify UART Clock and Reset (if required)

You may need to enable the clock in:

```dts
clocks {
    uart_clk: uart_clk {
        compatible = "fixed-clock";
        #clock-cells = <0>;
        clock-frequency = <48000000>;
    };
};
```

And enable it in the Clock Controller Driver.

---

## 📟 5. Debug Console Setup (optional)

If you're setting this UART as the kernel console:

### Add to bootargs:

```dts
chosen {
    bootargs = "console=ttyS2,115200 earlycon";
};
```

Or pass in U-Boot:

```bash
setenv bootargs 'console=ttyS2,115200 earlycon'
```

Where `ttyS2` maps to UART3 depending on kernel UART indexing.

---

## 🧪 6. Testing from User Space

Use serial tools:

```bash
minicom -D /dev/ttyS2
# OR
echo "hello" > /dev/ttyS2
```

Check available UARTs:

```bash
dmesg | grep tty
```

---

## 🧰 7. Debugging Tips

| Tool                          | Purpose                                       |
| ----------------------------- | --------------------------------------------- |
| `dmesg`                       | Check driver load and serial port init        |
| `cat /proc/tty/driver/serial` | List UART ports                               |
| `strace`                      | Debug user-level I/O                          |
| Oscilloscope                  | Check if TX/RX lines are toggling             |
| Loopback test                 | Connect TX ↔ RX and test echo                 |
| `devmem2`                     | Read/write UART registers for low-level debug |

---

## 🛠️ 8. What If It's a Custom UART?

If your SoC has a **vendor-specific UART IP**:

* Write a **new UART driver** or extend existing `8250.c`, `dw_apb_uart.c`, or vendor’s driver.
* Register the new port with the Linux **serial core** (`uart_register_driver`, `uart_add_one_port`).

Example skeleton:

```c
static struct uart_driver my_uart_driver = {
    .owner = THIS_MODULE,
    .driver_name = "my_uart",
    .dev_name = "ttyMY",
    .major = 204,
    .minor = 64,
    .nr = 1,
};

uart_register_driver(&my_uart_driver);
```

---

## ✅ Summary

| Step          | Description                            |
| ------------- | -------------------------------------- |
| Device Tree   | Define memory, IRQ, pinmux, and clocks |
| Kernel Config | Enable the correct UART driver         |
| Clock/Reset   | Ensure clocks are enabled              |
| Bootargs      | Set kernel console if required         |
| Test          | Use `/dev/ttySx` and `minicom`         |
| Debug         | Use `dmesg`, `devmem2`, oscilloscope   |

---

If you provide the **vendor name**, **SoC**, or **UART base address**, I can help you write the full **Device Tree**, **Kconfig**, and even register-level debug scripts. Let me know!
-----2