# Linux Device Driver `probe()` Function — Detailed Design Document

> **Audience:** Embedded / Linux kernel engineers and interview preparation.
> **Focus:** Platform drivers (Device Tree), with notes on I²C / SPI / PCI / USB.
> **Scope:** Why probe is needed, when and where it runs, what happens *before* and
> *after* it, how it works internally, how to write it, and how to debug it when it fails.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Why is `probe()` Needed?](#2-why-is-probe-needed)
3. [When is `probe()` Called?](#3-when-is-probe-called)
4. [Where is `probe()` Used?](#4-where-is-probe-used)
5. [What Happens *Before* `probe()`?](#5-what-happens-before-probe)
6. [How `probe()` Works Internally](#6-how-probe-works-internally)
7. [How to Write a `probe()` Function](#7-how-to-write-a-probe-function)
8. [Error Handling & Resource Cleanup](#8-error-handling--resource-cleanup)
9. [Deferred & Asynchronous Probing](#9-deferred--asynchronous-probing)
10. [What Happens *After* `probe()`?](#10-what-happens-after-probe)
11. [Manual Bind / Unbind Controls](#11-manual-bind--unbind-controls)
12. [Why a `probe()` Fails — Common Causes](#12-why-a-probe-fails--common-causes)
13. [Debugging a Failed `probe()`](#13-debugging-a-failed-probe)
14. [Best Practices Summary](#14-best-practices-summary)
15. [Quick Interview Q&A Cues](#15-quick-interview-qa-cues)

---

## 1. Introduction

The **`probe()`** function is one of the most important callbacks in the Linux device
driver model.

Whenever the kernel detects a hardware device and finds a matching driver, it invokes
that driver's **`probe()`** callback.

> **One-line definition:** `probe()` is the kernel calling your driver's callback to
> **bind** the driver to a specific device and **initialize** that device's resources so
> it becomes usable.

Think of `probe()` as *"the driver's entry point for a specific hardware device."*
Without it, the driver cannot initialize hardware, allocate resources, register
interfaces, or make the device usable.

### The Actors (Device Model)

| Actor | Structure | Role |
| --- | --- | --- |
| **Device** | `struct device` | A physical/peripheral instance (from DT, PCI, USB, hotplug). |
| **Driver** | `struct device_driver` (wrapped by `platform_driver`, `i2c_driver`, …) | Holds `probe()`/`remove()` callbacks and match tables. |
| **Bus** | `struct bus_type` | Enumerates devices and matches them to drivers (platform, i2c, spi, pci, usb). |
| **Userspace** | `udev` / `modprobe` | Listens for kernel uevents and may auto-load driver modules. |

---

## 2. Why is `probe()` Needed?

### The Problem

Linux must support a very dynamic hardware environment:

* Hot-plug devices (USB, PCIe)
* Dynamically loaded driver modules
* Multiple devices served by the **same** driver
* Device Tree–based platforms (SoC/embedded)
* ACPI-based platforms (x86)
* PCI / USB / I²C / SPI buses

The kernel cannot hard-code initialization for every device. It needs a **generic,
late-binding mechanism**:

```text
Device Found
      ↓
Find Matching Driver
      ↓
Call Driver Initialization   ← this callback is probe()
```

### What `probe()` is Responsible For

| Responsibility | Typical API |
| --- | --- |
| Allocate per-device driver state | `devm_kzalloc()` |
| Map MMIO registers | `devm_ioremap_resource()` |
| Enable clocks | `devm_clk_get()` + `clk_prepare_enable()` |
| Enable power | `devm_regulator_get()` + `regulator_enable()` |
| Register interrupts | `devm_request_irq()` / `devm_request_threaded_irq()` |
| Configure GPIOs / DMA | `devm_gpiod_get()`, `dma_set_mask_and_coherent()` |
| Register a char device | `cdev_add()` / `device_create()` |
| Register with a subsystem | `input_register_device()`, `register_netdev()`, … |
| Enable runtime PM | `pm_runtime_enable()` |

The single guiding idea: **`probe()` brings one device instance from "matched but dead"
to "fully operational."**

---

## 3. When is `probe()` Called?

`probe()` runs **only after a successful device ↔ driver match**. There are three
typical timing scenarios:

### Case 1 — Driver Loaded *After* the Device Exists

```bash
insmod my_driver.ko
```

```text
Device already present
Driver loaded
Match found
probe() called
```

### Case 2 — Device Appears *After* the Driver is Loaded

Example: a USB device is plugged in while the driver is already resident.

```text
Driver already loaded
USB device plugged in
Match found
probe() called
```

### Case 3 — Device Tree Boot

```text
Kernel boot
      ↓
Parse Device Tree (DTB)
      ↓
Create device objects, register on bus
      ↓
Register driver (built-in or module init)
      ↓
Match device ↔ driver
      ↓
probe() called
```

---

## 4. Where is `probe()` Used?

Almost every Linux bus framework exposes a `probe()` callback. This document centers on
the **platform driver**, but the concept is universal.

### Platform Driver (primary focus)

```c
static struct platform_driver my_driver = {
    .probe  = my_probe,
    .remove = my_remove,
    .driver = {
        .name           = "mydev",
        .of_match_table = my_of_match,
    },
};
module_platform_driver(my_driver);
```

Used for SoC-integrated devices: GPIO, UART, watchdog, RTC, I²C/SPI controllers, etc.

### Cross-Bus Summary

| Bus | Driver struct | Primary match mechanism |
| --- | --- | --- |
| Platform | `platform_driver` | DT `compatible` (`of_match_table`), ACPI IDs, name/id table |
| I²C | `i2c_driver` | `i2c_device_id` table + DT `compatible` |
| SPI | `spi_driver` | `spi_device_id` table + DT `compatible` |
| PCI | `pci_driver` | Vendor/Device/Subsystem IDs (`pci_device_id`) |
| USB | `usb_driver` | Vendor/Product/Class (`usb_device_id`) |

> **Note:** For auto-loading modules, every bus driver should publish its match table
> via `MODULE_DEVICE_TABLE(...)` so userspace `modprobe` can resolve the modalias.

---

## 5. What Happens *Before* `probe()`?

Understanding the pre-probe sequence is essential for both real debugging and interviews.

### Step 1 — Driver Registration

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

### Step 2 — Device Registration

From the Device Tree:

```dts
my_device@1000 {
    compatible = "vendor,my-device";
    reg = <0x1000 0x100>;
    interrupts = <0 42 4>;
};
```

The kernel creates a `struct platform_device` for this node.

### Step 3 — Device Added to the Bus

The `platform_bus` maintains a **device list** and a **driver list**.

### Step 4 — Matching Begins

```c
driver_attach()
```

The bus-specific match routine runs. For the platform bus that is `platform_match()`,
which checks (in order):

```text
OF compatible string  →  ACPI table  →  id_table  →  driver name
```

Example match:

```dts
/* Device Tree */
compatible = "vendor,my-device";
```

```c
/* Driver */
static const struct of_device_id my_of_match[] = {
    { .compatible = "vendor,my-device" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, my_of_match);
```

→ **Match succeeds.**

### Step 5 — Bind the Driver to the Device

Once matched, the kernel proceeds to `really_probe()`, which ultimately calls your
`probe()`.

---

## 6. How `probe()` Works Internally

### Kernel Call Chain (simplified)

```text
Device Registration
       ↓
device_add()  →  bus_add_device()
       ↓
Driver Registration
       ↓
driver_register()  →  bus_add_driver()
       ↓
driver_attach()
       ↓
__driver_attach()
       ↓
driver_match_device()  →  platform_match()
       ↓
driver_probe_device()
       ↓
really_probe()
       ↓
call_driver_probe()
       ↓
driver->probe()      ←  YOUR my_probe() RUNS HERE
```

### Narrative (real-world example)

Consider a touchscreen controller (e.g., the Goodix I²C controller) on an embedded board:

1. **Boot / enumeration.** During boot the kernel parses the DTB, discovers the
   controller node, creates a device structure, and registers it on the **I²C bus**.
2. **Driver registration.** When the driver's `init` runs, it registers itself with its
   bus subsystem.
3. **Matching phase.** Driver registration triggers matching: the kernel iterates all
   devices on the bus and compares each against the driver using multiple mechanisms —
   Device Tree `compatible` strings, ACPI IDs, or bus-specific ID tables. This flexible
   approach lets the *same* driver work across different firmware interfaces.
4. **Probe invocation.** On a match, the kernel calls the driver's `probe()`. This is
   where the driver requests every resource it needs — MMIO regions, interrupt lines,
   clocks, regulators, GPIO pins, DMA channels — performs hardware-specific init, and
   registers the device with a higher-level subsystem (input, network, block, …).

> **Key property:** `probe()` runs in **process context**, so it *may sleep* and perform
> blocking operations (firmware download, calibration, I²C transfers, etc.).

> **Go deeper:** For a source-level walk of every function in this chain (`really_probe`,
> `call_driver_probe`, the deferred-probe engine, async probe, and locking), see
> [Deep/03_Probe_Internals.md](Deep/03_Probe_Internals.md).

---

## 7. How to Write a `probe()` Function

A robust platform `probe()` typically performs these steps in order:

1. **Allocate driver state** — `devm_kzalloc()`, then `platform_set_drvdata()`.
2. **Map MMIO** — `platform_get_resource()` + `devm_ioremap_resource()`.
3. **Acquire clocks** — `devm_clk_get()` + `clk_prepare_enable()`
   (return `-EPROBE_DEFER` if not ready).
4. **Acquire regulators / GPIOs** — `devm_regulator_get()`, `devm_gpiod_get()`.
5. **Set up DMA** — `dma_set_mask_and_coherent()`, `dma_alloc_coherent()`.
6. **Request IRQs** — `platform_get_irq()` + `devm_request_irq()` /
   `devm_request_threaded_irq()`. Keep the top half minimal.
7. **Initialize hardware** to a known state (reset, firmware upload if needed).
8. **Register kernel interfaces** — char device, input/net/block subsystem, sysfs
   attribute groups.
9. **Enable runtime PM** — `pm_runtime_enable()` if supported.
10. **Return 0** on success (any non-zero return unbinds the driver).

### Complete Platform Driver Example

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/interrupt.h>

struct mydev_priv {
    void __iomem *base;   /* mapped MMIO region */
    struct clk   *clk;
    int           irq;
};

static irqreturn_t mydev_thread_fn(int irq, void *dev_id)
{
    struct mydev_priv *priv = dev_id;

    /* Bottom-half work: safe to sleep here. */
    (void)priv;
    return IRQ_HANDLED;
}

static int mydev_probe(struct platform_device *pdev)
{
    struct device     *dev = &pdev->dev;
    struct mydev_priv *priv;
    struct resource   *res;
    int                ret;

    dev_info(dev, "probe called\n");

    /* 1. Allocate driver private data (auto-freed on remove). */
    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    platform_set_drvdata(pdev, priv);

    /* 2. Map MMIO registers from the DT 'reg' property. */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    priv->base = devm_ioremap_resource(dev, res);
    if (IS_ERR(priv->base))
        return PTR_ERR(priv->base);

    /* 3. Acquire and enable the clock (may defer). */
    priv->clk = devm_clk_get(dev, NULL);
    if (IS_ERR(priv->clk)) {
        ret = PTR_ERR(priv->clk);
        if (ret == -EPROBE_DEFER)
            return -EPROBE_DEFER;          /* retry later */
        dev_err(dev, "failed to get clock: %d\n", ret);
        return ret;
    }
    ret = clk_prepare_enable(priv->clk);
    if (ret)
        return ret;

    /* 4. Request a threaded IRQ. */
    priv->irq = platform_get_irq(pdev, 0);
    if (priv->irq < 0) {
        ret = priv->irq;
        goto err_disable_clk;
    }
    ret = devm_request_threaded_irq(dev, priv->irq, NULL,
                                    mydev_thread_fn,
                                    IRQF_ONESHOT,
                                    dev_name(dev), priv);
    if (ret) {
        dev_err(dev, "failed to request IRQ %d: %d\n", priv->irq, ret);
        goto err_disable_clk;
    }

    /* 5. Initialize hardware and register interfaces here. */

    dev_info(dev, "device ready\n");
    return 0;

err_disable_clk:
    clk_disable_unprepare(priv->clk);
    return ret;
}

static int mydev_remove(struct platform_device *pdev)
{
    struct mydev_priv *priv = platform_get_drvdata(pdev);

    /* Reverse of probe: put device in a quiescent state. */
    clk_disable_unprepare(priv->clk);
    dev_info(&pdev->dev, "remove called\n");
    return 0;
}

static const struct of_device_id mydev_of_match[] = {
    { .compatible = "myvendor,mydev" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mydev_of_match);

static struct platform_driver mydev_driver = {
    .probe  = mydev_probe,
    .remove = mydev_remove,
    .driver = {
        .name           = "mydev",
        .of_match_table = mydev_of_match,
    },
};
module_platform_driver(mydev_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Example platform driver demonstrating probe()");
```

> **Note on `clk` cleanup:** `clk_prepare_enable()` has no `devm_` auto-disable
> counterpart, so the example uses an explicit `goto` unwind and disables the clock in
> `remove()`. Pure-`devm` resources (memory, ioremap, IRQ) need no manual cleanup.

---

## 8. Error Handling & Resource Cleanup

There are two idiomatic styles. Prefer **(A)** wherever possible.

### A. `devm_*` Managed Resources (recommended)

`devm_kzalloc`, `devm_ioremap_resource`, `devm_request_irq`, `devm_clk_get`, etc. are
automatically released when the device is removed or the module is unloaded. On probe
failure you can simply `return <err>` and the core unwinds everything for you.

### B. Manual Cleanup with `goto` Labels

When a resource has no managed variant (or needs ordered teardown), use the classic
reverse-order unwind pattern:

```c
data = kzalloc(sizeof(*data), GFP_KERNEL);
if (!data)
    return -ENOMEM;

base = ioremap(res->start, resource_size(res));
if (!base) {
    ret = -ENOMEM;
    goto err_free;
}

ret = request_irq(irq, isr, 0, "mydev", data);
if (ret)
    goto err_iounmap;

return 0;            /* success */

err_iounmap:
    iounmap(base);
err_free:
    kfree(data);
    return ret;
```

> **Rule:** A failed `probe()` must leave the system exactly as it found it — no leaked
> memory, mappings, IRQs, clocks, or regulators.

---

## 9. Deferred & Asynchronous Probing

### `-EPROBE_DEFER`

If `probe()` discovers that a dependency is not yet available — a regulator, clock, PHY,
GPIO provider, or parent device that probes later — it must return **`-EPROBE_DEFER`**.

```c
priv->vdd = devm_regulator_get(dev, "vdd");
if (IS_ERR(priv->vdd)) {
    ret = PTR_ERR(priv->vdd);
    if (ret == -EPROBE_DEFER)
        return -EPROBE_DEFER;   /* core re-queues this device */
    return ret;
}
```

The driver core places the device on a **deferred probe list** and retries it later,
each time a new driver or device is registered. This elegantly resolves inter-driver
dependency ordering without manual sequencing.

### Asynchronous Probe

Drivers with long initialization (firmware download, calibration) can opt into async
probing to speed up boot:

```c
static struct platform_driver mydev_driver = {
    .driver = {
        .name        = "mydev",
        .probe_type  = PROBE_PREFER_ASYNCHRONOUS,
    },
    .probe = mydev_probe,
};
```

The probe then runs in a worker context rather than blocking the boot path.

---

## 10. What Happens *After* `probe()`?

### On Success (`probe()` returned 0)

* The device is now **bound** to the driver. You can confirm this in sysfs:
  `/sys/bus/platform/devices/<dev>/driver` becomes a symlink to the driver.
* The pointer stored via `platform_set_drvdata()` / `dev_set_drvdata()` is available to
  every other callback (IRQ handler, file ops, PM hooks).
* Any interface registered in probe is now live: `/dev/<node>` for char devices, a
  network interface, an input device, sysfs attributes, etc.
* Userspace can interact with the device through `/dev`, sysfs, or netlink.

### On Failure (`probe()` returned non-zero)

* `-EPROBE_DEFER` → the device is re-queued and probe will be retried later.
* Any other error → the core unwinds managed resources and the device stays **unbound**;
  the driver is not associated with it.

### Lifecycle: `remove()` / Unbind

When the device is removed (hot-unplug), the driver is unloaded, or the device is
manually unbound, the kernel calls `remove()`:

```text
probe()   →  device bound & operational
   ⋮          (normal operation)
remove()  →  device quiesced, resources released, interfaces unregistered
```

`remove()` is essentially the mirror image of `probe()`: stop hardware, free IRQs,
disable clocks/regulators, unregister subsystem interfaces. With `devm_*` resources,
most of this is automatic.

---

## 11. Manual Bind / Unbind Controls

You can drive binding by hand through sysfs — invaluable for debugging:

```bash
# List devices and drivers on the platform bus
ls /sys/bus/platform/devices/
ls /sys/bus/platform/drivers/

# Force-bind a device to a driver (triggers probe)
echo -n <device-name> > /sys/bus/platform/drivers/<driver-name>/bind

# Unbind a device (triggers remove)
echo -n <device-name> > /sys/bus/platform/drivers/<driver-name>/unbind

# Watch uevents flow as you insert/remove modules
udevadm monitor --kernel --udev
```

---

## 12. Why a `probe()` Fails — Common Causes

| Symptom | Likely Cause | Fix / Check |
| --- | --- | --- |
| `probe()` never called | DT `compatible` mismatch | Compare DT node vs `of_match_table` |
| Module not auto-loaded | Missing modalias | Add `MODULE_DEVICE_TABLE(...)` |
| Probe retried repeatedly | Returned `-EPROBE_DEFER` | Ensure provider (regulator/clock/PHY) probes |
| `ioremap` / resource error | Wrong `reg` range in DT | Verify addresses in `dmesg` and DT |
| IRQ request fails | Wrong IRQ number/flags | Check `interrupts` property, trigger flags |
| DMA alloc fails | DMA mask not set | Call `dma_set_mask_and_coherent()` |
| Firmware load fails | File missing | `request_firmware()` returns `-ENOENT`; install firmware |
| Device unusable after probe | Permissions / udev rule | Inspect `udevadm monitor` output |

---

## 13. Debugging a Failed `probe()`

A practical, ordered checklist:

### Step 1 — Read the Kernel Log

```bash
dmesg | grep -i 'probe\|<driver>\|<device>'
```

Look for your `dev_info`/`dev_err` lines, `-EPROBE_DEFER` notices, `ioremap` failures, or
IRQ errors. Always log with `dev_name(&pdev->dev)` so messages are easy to correlate.

### Step 2 — Confirm the Device & Match Tables

```bash
# Is the device present on the bus?
ls /sys/bus/platform/devices/

# What modalias / compatible is the device exposing?
cat /sys/bus/platform/devices/<dev>/uevent
cat /sys/bus/platform/devices/<dev>/modalias

# Is the driver registered?
ls /sys/bus/platform/drivers/
```

If the device exists but no `driver` symlink appears under it, the **match failed** —
re-check the `compatible` string on both sides.

### Step 3 — Watch the Uevent / Module Flow

```bash
udevadm monitor --kernel --udev
# then insmod / plug the device and observe
```

### Step 4 — Verify Allocated Resources

```bash
cat /proc/interrupts          # confirm the IRQ was assigned
cat /proc/iomem               # confirm MMIO region is reserved
```

### Step 5 — Force a Manual Bind

```bash
echo -n <device-name> > /sys/bus/platform/drivers/<driver-name>/bind
dmesg | tail
```

This re-runs `probe()` immediately and surfaces the exact failure point in `dmesg`.

### Step 6 — Inspect Deferred Probes

If probe keeps deferring, the missing supplier hasn't probed yet. On kernels with
debugfs:

```bash
cat /sys/kernel/debug/devices_deferred
```

This lists devices currently waiting on `-EPROBE_DEFER`.

---

## 14. Best Practices Summary

* Use `devm_*` helpers for most allocations to simplify (and guarantee) cleanup.
* Return `-EPROBE_DEFER` when a dependency isn't ready — never busy-wait.
* Keep IRQ top halves minimal; push heavy work to threaded IRQs or workqueues.
* Never block forever in `probe()`; use timeouts or async/work-deferred init.
* Log with `dev_info()` / `dev_err()` and `dev_name(&pdev->dev)` for easy `dmesg`
  correlation.
* Make `remove()` the exact mirror of `probe()`.
* Publish `MODULE_DEVICE_TABLE(...)` so userspace can auto-load the module.
* Document the DT binding under `Documentation/devicetree/bindings/...`.

---

## 15. Quick Interview Q&A Cues

* **Q: When is `probe()` called?** After a successful device ↔ driver match (driver-loaded-after-device, device-appears-after-driver, or DT boot enumeration).
* **Q: What context does `probe()` run in?** Process context — it may sleep.
* **Q: Where are resources requested?** In `probe()`, *not* in module `init()`.
* **Q: What does returning 0 vs non-zero mean?** 0 binds the driver; non-zero unbinds; `-EPROBE_DEFER` requeues for retry.
* **Q: How does cleanup work?** `devm_*` auto-releases on remove/failure; otherwise use ordered `goto` unwind.
* **Q: Trace the internal call flow.** `driver_attach` → `__driver_attach` → `driver_match_device`/`platform_match` → `driver_probe_device` → `really_probe` → `call_driver_probe` → `driver->probe()`.
* **Q: How do you debug a probe that never runs?** Check the DT `compatible` vs `of_match_table`, inspect `/sys/bus/.../devices/<dev>/uevent`, and watch `dmesg`.

---

### Related Documents

* [01_Probe_Internally.md](01_Probe_Internally.md) — sections 1–6 source material (Why / When / Where / Before / Internal flow).
* [Deep/01_ProbeCallFlow.md](Deep/01_ProbeCallFlow.md) — Goodix touchscreen call-flow narrative.
* [Deep/02_Probe.md](Deep/02_Probe.md) — extended notes on debugging, `-EPROBE_DEFER`, and cleanup patterns.
* [Deep/03_Probe_Internals.md](Deep/03_Probe_Internals.md) — kernel source-level deep dive of the internal probe machinery (`dd.c`).
