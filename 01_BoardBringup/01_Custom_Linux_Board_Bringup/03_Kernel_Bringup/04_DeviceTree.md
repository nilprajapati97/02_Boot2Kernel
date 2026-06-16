In **Embedded Linux kernel bring-up**, the **Device Tree Source (`.dts`)** is a **crucial component**. It tells the Linux kernel **what hardware exists** on the board and **how to initialize it** (e.g., UART, MMC, Ethernet, etc.).

---

## ✅ What is the Device Tree?
==========================================================================================================================
The **Device Tree (DT)** is a data structure that describes the **hardware layout** of your board **outside the kernel source code**. It is passed from U-Boot to the kernel at boot time.

---

## 🧱 Block-Wise Explanation

### 🧱 Block 1: Boot Flow with Device Tree

```text
+----------------+       +---------------+       +-------------------+
| U-Boot / SPL   | --->  | Linux Kernel  | --->  | Device Drivers    |
+----------------+       +---------------+       +-------------------+
        \                                        /
         \----> Passes DTB (compiled .dts) ---->/
```

* U-Boot loads both **kernel image (`zImage`/`Image`)** and **Device Tree Blob (`.dtb`)**
* Kernel uses DTB to probe and initialize devices

---

## 🧱 Block 2: Typical DTS File Structure
==========================================================================================================================
Here’s a minimal `.dts` example (e.g., `myboard.dts`):

```dts
/dts-v1/;
/ {
    model = "My Embedded Board";
    compatible = "myvendor,myboard";

    chosen {
        bootargs = "console=ttyS0,115200";
    };

    aliases {
        serial0 = &uart0;
    };

    memory@80000000 {
        device_type = "memory";
        reg = <0x80000000 0x10000000>; // 256MB RAM
    };

    soc {
        #address-cells = <1>;
        #size-cells = <1>;
        compatible = "simple-bus";
        ranges;

        uart0: serial@10000000 {
            compatible = "ns16550a";
            reg = <0x10000000 0x1000>;
            clock-frequency = <24000000>;
            current-speed = <115200>;
            status = "okay";
        };

        mmc0: mmc@10005000 {
            compatible = "mmc-controller";
            reg = <0x10005000 0x1000>;
            bus-width = <4>;
            status = "okay";
        };
    };
};
```

---

## 🧱 Block 3: Key Nodes to Enable
==========================================================================================================================
| Device   | Node Sample                  |
| -------- | ---------------------------- |
| UART     | `serial@10000000`            |
| MMC/SD   | `mmc@xxxxxxx`                |
| Ethernet | `ethernet@xxxxxx`            |
| I2C/SPI  | `i2c@xxxxxx` or `spi@xxxxxx` |
| GPIO     | `gpio@xxxxxxx`               |
| Memory   | `memory@80000000`            |

---

## 🧱 Block 4: DTS → DTB Compilation
==========================================================================================================================
Compile using:

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- myboard_defconfig
make myboard.dtb
```

The `dtc` (Device Tree Compiler) converts `.dts` → `.dtb`.

---

## 🧱 Block 5: U-Boot Integration
==========================================================================================================================
U-Boot loads the DTB alongside the kernel:

```bash
load mmc 0:1 ${fdt_addr_r} myboard.dtb
load mmc 0:1 ${kernel_addr_r} Image
booti ${kernel_addr_r} - ${fdt_addr_r}
```

U-Boot must be configured with:

```bash
CONFIG_OF_CONTROL=y
CONFIG_DEFAULT_DEVICE_TREE="myboard"
```

---

## 🧱 Block 6: Kernel Logs on DT Probe
==========================================================================================================================
When Linux boots, you'll see DT-related logs:

```log
OF: fdt:Machine model: My Embedded Board
mmc0: new high speed SDHC card at address 0001
Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled
```

---

## 🧱 Block 7: Driver Matching (`compatible` → probe())
==========================================================================================================================
```dts
serial@10000000 {
    compatible = "ns16550a";
    ...
};
```

This matches the driver:

```c
static const struct of_device_id serial_of_match[] = {
    { .compatible = "ns16550a", .data = &uart_platform_data },
    { }
};

MODULE_DEVICE_TABLE(of, serial_of_match);

static struct platform_driver serial_driver = {
    .probe  = serial_probe,
    ...
};
```

✅ When match found: `serial_probe()` gets called, registers UART.
==========================================================================================================================
---

## ✅ Summary Table

| Item              | Purpose                         | Notes                            |
| ----------------- | ------------------------------- | -------------------------------- |
| `.dts`            | Human-readable Device Tree file | Board-specific                   |
| `.dtb`            | Compiled binary DT for boot     | Passed to kernel by U-Boot       |
| `compatible`      | Matches device with driver      | Triggers `.probe()`              |
| `reg`             | MMIO address of peripheral      | Must match SoC layout            |
| `status = "okay"` | Enables a device in DT          | `"disabled"` → not probed        |
| `bootargs`        | Kernel arguments from DT        | Used if not overridden in U-Boot |

---

## 📌 Real-World Device Tree Example (Qualcomm SDM660)
==========================================================================================================================
```dts
&blsp1_uart2 {
    status = "okay";
    pinctrl-0 = <&blsp1_uart2_default>;
    pinctrl-names = "default";
};
```

Corresponding in `qcom-sdm660.dtsi`:

```dts
blsp1_uart2: serial@78b0000 {
    compatible = "qcom,geni-debug-uart";
    reg = <0x78b0000 0x2000>;
    ...
};
```

---

Would you like a **full real-world `.dts` file + DTB log + driver match trace** for SDM660 or STM32MP157 to visualize the flow?
