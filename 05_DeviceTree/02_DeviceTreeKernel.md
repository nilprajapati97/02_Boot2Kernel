Sure! Let's walk through **how the Linux kernel uses the Device Tree**, step-by-step, **from an embedded Linux system developer’s point of view**, starting from scratch.

---

## 📌 What is Device Tree?

A **Device Tree (DT)** is a data structure for describing hardware that is **not discoverable** (like I2C devices, GPIOs, UARTs, regulators, etc). It's used during kernel boot to **pass hardware details** to the kernel in embedded systems.

---

## 📍 Why is Device Tree Needed?

Unlike a PC (which uses ACPI or BIOS for device discovery), embedded boards like ARM-based SoCs (Qualcomm, NXP, TI) **lack automatic hardware discovery**.

So:

* We need to tell the kernel **what devices exist**
* What addresses, interrupts, clocks, regulators, buses they use

This is done using the **Device Tree Blob (DTB)** passed to the kernel at boot.

---

## 🧩 Device Tree Workflow (Embedded Linux)

```
+----------------------+
| .dts or .dtsi Source |
+----------+-----------+
           |
           v
+----------------------+
|    dtc (compiler)    | ← device tree compiler
+----------+-----------+
           |
           v
+----------------------+
|   .dtb binary blob   | ← passed to kernel at boot (by U-Boot)
+----------+-----------+
           |
           v
+-----------------------------+
| Linux kernel parses .dtb    |
| and initializes subsystems  |
+-----------------------------+
```

---

## 🔍 Kernel Flow: How It Uses DT

Let's see this **internally**.

### 🔹 1. **Bootloader loads the DTB**

* U-Boot or similar passes the DTB address to the kernel:

```c
// arm64
void __init setup_arch(...) {
    __fdt_pointer = initial_boot_params; // DTB base address
}
```

---

### 🔹 2. **Kernel parses the DTB**

In `start_kernel()`, kernel calls:

```c
setup_arch() → early_init_dt_scan() → unflatten_device_tree()
```

This:

* Parses the **flat DTB blob**
* Builds **internal kernel structures** (`struct device_node`, etc.)

---

### 🔹 3. **Platform/Bus Setup**

For each enabled node in DT:

* The kernel **matches the "compatible" string** to a registered driver
* Probes the driver using the **platform/device model**

Example:

```dts
&i2c_2 {
  bmi160@68 {
    compatible = "bosch,bmi160";
    reg = <0x68>;
    interrupt-parent = <&tlmm>;
    interrupts = <23>;
  };
};
```

* Kernel i2c-core registers this device node
* When a driver calls `i2c_add_driver()`, it matches `"bosch,bmi160"` and probes the device

---

### 🔹 4. **Driver Probing and Resource Extraction**

Inside the probe function (e.g., `bmi160_probe()`):

```c
int irq = of_irq_get(dev->of_node, 0);
struct regulator *vdd = devm_regulator_get(&client->dev, "vdd");
```

* Kernel extracts IRQ, GPIOs, clocks, voltages from DT node
* Binds this info to the device

---

## 🔧 Example Flow: Adding a New Device

Suppose we have an I2C device on I2C bus 1 at address 0x68:

### Step 1: Add in `.dts`

```dts
&i2c_1 {
    accel@68 {
        compatible = "bosch,bmi160";
        reg = <0x68>;
        interrupt-parent = <&tlmm>;
        interrupts = <42 IRQ_TYPE_EDGE_RISING>;
        vdd-supply = <&vreg_l7>;
    };
};
```

---

### Step 2: Compile

```bash
dtc -I dts -O dtb -o yourboard.dtb yourboard.dts
```

---

### Step 3: Boot + Kernel Logs

During boot:

```plaintext
i2c i2c-1: new_device: Instantiated device accel at 0x68
bmi160: probing device...
```

This means:

* DT matched the driver
* Driver got probed
* IRQ, power, etc., are used

---

## 🧠 Kernel APIs Used for DT

| Purpose         | Function                              |
| --------------- | ------------------------------------- |
| Get node        | `of_find_node_by_name()`              |
| Get property    | `of_property_read_u32()`, `_string()` |
| Get child nodes | `for_each_child_of_node()`            |
| Get IRQ         | `of_irq_get()`                        |
| Get GPIO        | `devm_gpiod_get()`                    |
| Get regulators  | `devm_regulator_get()`                |

---

## 📚 Kernel Code Internals

* `drivers/of/` → Core device tree parser
* `drivers/base/platform.c` → Platform device/driver binding
* `arch/arm64/boot/dts/` → Reference board DTS files
* `Documentation/devicetree/` → Device tree bindings

---

## ✅ Summary

| Step | What Happens                                            |
| ---- | ------------------------------------------------------- |
| 1    | Bootloader loads `.dtb` into memory                     |
| 2    | Kernel reads `.dtb` and builds internal tree structures |
| 3    | Kernel matches "compatible" strings to drivers          |
| 4    | Driver probes and extracts DT info                      |
| 5    | Device gets initialized with resources like IRQ, I2C    |

---

Would you like a visual flowchart or want to trace a specific driver (like GPIO, UART, or custom sensor) through the device tree?
