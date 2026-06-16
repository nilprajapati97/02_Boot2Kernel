# Linux Device Driver Probe Function - Detailed Design Document

## 1. Introduction

The **probe()** function is one of the most important callbacks in Linux device driver architecture.

Whenever Linux detects a hardware device and finds a matching driver, it invokes the driver's **probe()** function.

Think of `probe()` as:

> "The driver's entry point for a specific hardware device."

Without probe(), the driver cannot initialize hardware, allocate resources, register interfaces, or make the device usable.

---

# 2. Why is Probe Needed?

## Problem

Linux supports:

* Hot-plug devices
* Dynamic driver loading
* Multiple devices using same driver
* Device Tree based platforms
* ACPI based platforms
* PCI/USB/I2C/SPI devices

The kernel cannot initialize every device manually.

It needs a generic mechanism:

```text
Device Found
      ↓
Find Matching Driver
      ↓
Call Driver Initialization
```

This initialization callback is:

```c
probe()
```

---

## Purpose of Probe

Probe is responsible for:

### Device Initialization

```c
Initialize Hardware Registers
```

### Memory Allocation

```c
Allocate Driver Data
```

### Interrupt Registration

```c
request_irq()
```

### Clock Configuration

```c
clk_prepare_enable()
```

### Power Management Setup

```c
regulator_enable()
```

### Character Device Registration

```c
cdev_add()
```

### Sysfs Interface Creation

```c
device_create()
```

---

# 3. When is Probe Called?

Probe is called when:

## Case 1: Driver Loaded After Device Exists

Example:

```bash
insmod my_driver.ko
```

Device already exists.

Kernel:

```text
Device Found
Driver Loaded
Matching Successful
Probe Called
```

---

## Case 2: Device Appears After Driver Loaded

Example:

USB device plugged.

```text
Driver Already Loaded
USB Device Plugged
Match Found
Probe Called
```

---

## Case 3: Device Tree Based Platform

Boot Process:

```text
Kernel Boot
      ↓
Parse Device Tree
      ↓
Create Device Objects
      ↓
Register Driver
      ↓
Match Device
      ↓
Probe Called
```

---

# 4. Where is Probe Used?

Almost every Linux bus framework.

---

## Platform Driver

```c
static struct platform_driver my_driver = {
    .probe = my_probe,
    .remove = my_remove,
};
```

Used for:

* SoC devices
* GPIO
* UART
* Watchdog
* RTC

---

## I2C Driver

```c
static struct i2c_driver my_driver = {
    .probe = my_i2c_probe,
};
```

---

## SPI Driver

```c
static struct spi_driver my_driver = {
    .probe = my_spi_probe,
};
```

---

## PCI Driver

```c
static struct pci_driver my_driver = {
    .probe = my_pci_probe,
};
```

---

## USB Driver

```c
static struct usb_driver my_driver = {
    .probe = my_usb_probe,
};
```

---

# 5. What Happens Before Probe?

Understanding this is critical for interviews.

---

## Step 1: Driver Registration

Driver registers itself.

Example:

```c
module_platform_driver(my_driver);
```

Internally:

```text
platform_driver_register()
        ↓
driver_register()
        ↓
bus_add_driver()
```

---

## Step 2: Device Registration

Device registered from:

### Device Tree

```dts
my_device@1000 {
    compatible = "vendor,my-device";
};
```

Kernel creates:

```c
struct platform_device
```

---

## Step 3: Device Added to Bus

```text
platform_bus
```

contains:

```text
Device List
Driver List
```

---

## Step 4: Matching Begins

Kernel executes:

```c
driver_attach()
```

---

### Matching Function

Platform Bus:

```c
platform_match()
```

Checks:

```text
Compatible String
Device Name
ID Table
ACPI Table
```

---

Example:

Device Tree:

```dts
compatible = "vendor,my-device";
```

Driver:

```c
static const struct of_device_id ids[] = {
    { .compatible = "vendor,my-device"},
};
```

Match Success.

---

## Step 5: Driver Bound to Device

Kernel:

```c
really_probe()
```

called.

---

# 6. Internal Probe Flow

Kernel Source:

```text
driver_probe_device()
       ↓
really_probe()
       ↓
call_driver_probe()
       ↓
driver->probe()
```

Your probe finally runs.

---

# Complete Internal Flow

```text
Device Registration
       ↓
device_add()
       ↓
bus_add_device()
       ↓
Driver Registration
       ↓
driver_register()
       ↓
bus_add_driver()
       ↓
driver_attach()
       ↓
__driver_attach()
       ↓
driver_match_device()
       ↓
platform_match()
       ↓
driver_probe_device()
       ↓
really_probe()
       ↓
call_driver_probe()
       ↓
my_probe()
```

Interviewers love this flow.

---

# 7. Example Probe Function

```c
static int my_probe(struct platform_device *pdev)
{
    struct my_priv *priv;

    priv = devm_kzalloc(&pdev->dev,
                        sizeof(*priv),
                        GFP_KERNEL);

    if (!priv)
        return -ENOMEM;

    platform_set_drvdata(pdev, priv);

    return 0;
}
```

---

# 8. Typical Activities Inside Probe

## Get Driver Data

```c
priv = devm_kzalloc(...)
```

---

## Map Registers

```c
res = platform_get_resource(pdev,
                            IORESOURCE_MEM,
                            0);

base = devm_ioremap_resource(...);
```

---

## Enable Clock

```c
clk = devm_clk_get(...)
clk_prepare_enable(clk);
```

---

## Enable Regulator

```c
regulator_enable(...)
```

---

## Request IRQ

```c
irq = platform_get_irq(...);

request_irq(...)
```

---

## Initialize Hardware

```c
writel(...)
```

---

## Register Char Device

```c
alloc_chrdev_region()

cdev_add()
```

---

## Create Sysfs

```c
device_create()
```

---

# 9. What Happens After Probe?

If Probe Returns:

```c
return 0;
```

Kernel considers device active.

---

Flow:

```text
Probe Success
      ↓
Driver Bound
      ↓
Device Operational
      ↓
User Space Access
```

---

Examples:

```bash
/dev/mydevice
```

appears.

Interrupts begin.

DMA starts.

Hardware works.

---

# 10. What If Probe Fails?

Example:

```c
return -ENOMEM;
```

or

```c
return -EINVAL;
```

Kernel:

```text
Device Exists
Driver Exists
Probe Failed
```

Device remains unbound.

---

Example dmesg:

```text
my_driver probe failed -22
```

---

# 11. Deferred Probe

Very common in Qualcomm/NVIDIA.

Example:

Driver requires regulator.

Regulator driver not loaded yet.

Probe:

```c
return -EPROBE_DEFER;
```

Kernel:

```text
Probe Deferred
Wait
Dependency Available
Probe Retry
```

---

Flow:

```text
Probe
 ↓
Clock Missing
 ↓
-EPROBE_DEFER
 ↓
Clock Driver Loads
 ↓
Probe Again
```

---

Check:

```bash
cat /sys/kernel/debug/devices_deferred
```

---

# 12. How Probe Works Internally

Kernel Function:

```c
really_probe()
```

Major actions:

```text
Lock Device
Set Driver
Pin Module
Call Probe
Check Return Value
Bind Device
Create Sysfs Links
```

Pseudo Flow:

```c
ret = drv->probe(dev);

if (!ret)
     device_bind();
else
     cleanup();
```

---

# 13. Debugging Probe Failures

## Method 1: Check dmesg

```bash
dmesg | grep probe
```

or

```bash
dmesg -w
```

---

## Method 2: Add dev_info()

```c
dev_info(dev, "probe start\n");
```

```c
dev_info(dev, "clock enabled\n");
```

```c
dev_info(dev, "probe success\n");
```

---

## Method 3: Dynamic Debug

Enable:

```bash
echo 'file * +p' \
 > /sys/kernel/debug/dynamic_debug/control
```

---

## Method 4: Check Device Tree Match

Verify:

```bash
cat /proc/device-tree/compatible
```

Compare with:

```c
of_match_table
```

---

## Method 5: Check Driver Binding

```bash
ls /sys/bus/platform/drivers/
```

---

Check:

```bash
ls /sys/bus/platform/devices/
```

---

## Method 6: Deferred Probe Debug

```bash
cat /sys/kernel/debug/devices_deferred
```

Example:

```text
my_device waiting for regulator
```

---

## Method 7: ftrace

Enable:

```bash
echo function > \
/sys/kernel/debug/tracing/current_tracer
```

Filter:

```bash
echo "*probe*" > \
set_ftrace_filter
```

---

## Method 8: Tracepoints

```bash
trace-cmd record \
-e driver
```

View:

```bash
trace-cmd report
```

---

## Method 9: Kernel Crash Analysis

If probe crashes:

```text
NULL Pointer
Invalid Register Access
Clock Failure
DMA Failure
```

Analyze:

* Ramdump
* crash utility
* Trace32
* KGDB

Backtrace:

```bash
bt
```

Look for:

```text
my_probe()
really_probe()
driver_probe_device()
```

---

# 14. Common Reasons Probe Fails

| Issue                    | Error              |
| ------------------------ | ------------------ |
| DT mismatch              | Probe never called |
| Memory allocation failed | -ENOMEM            |
| Clock missing            | -EPROBE_DEFER      |
| Regulator missing        | -EPROBE_DEFER      |
| IRQ unavailable          | -EINVAL            |
| Wrong register base      | Crash              |
| NULL pointer             | Oops               |
| Hardware absent          | -ENODEV            |
| Resource busy            | -EBUSY             |

---

# 15. Interview Answer (Short Version)

**What is Probe?**

Probe is a driver callback invoked by the Linux Driver Model when a device and driver successfully match. It initializes hardware, allocates resources, enables clocks/regulators, registers interrupts, and makes the device operational.

**Internal Flow:**

```text
Device Registration
    ↓
Driver Registration
    ↓
Bus Match Function
    ↓
driver_probe_device()
    ↓
really_probe()
    ↓
driver->probe()
```

**Before Probe:**

* Device created
* Driver registered
* Match successful

**After Probe Success:**

* Driver bound to device
* Hardware initialized
* Device operational

**Probe Failure Debugging:**

* dmesg
* dynamic_debug
* ftrace
* trace-cmd
* devices_deferred
* crash utility
* Trace32
* Device Tree verification

For Qualcomm/NVIDIA ARM64 interviews, understanding **`really_probe()`**, **deferred probe (`-EPROBE_DEFER`)**, **Device Tree matching**, and **resource management (`devm_* APIs`)** is considered essential.
