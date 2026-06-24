If a **character driver fails during probing**, the goal is to determine **why the driver's `probe()` function returned an error**. The `probe()` function is responsible for initializing the device after the kernel matches the driver with the hardware.

---

# Step 1: Understand the probe sequence

```text
Device detected
      │
      ▼
Bus (PCI/I2C/SPI/Platform/USB)
      │
      ▼
Driver matched
      │
      ▼
probe()
      │
      ├── Allocate resources
      ├── Map registers
      ├── Request IRQ
      ├── Initialize hardware
      ├── Register char device
      └── Return 0
```

If any step fails, `probe()` returns a negative error code.

---

# Step 2: Check kernel logs

The first place to look is the kernel log:

```bash
dmesg
```

or

```bash
dmesg | tail -100
```

Example:

```text
my_driver: probe started
my_driver: failed to request irq
my_driver: probe failed -22
```

If your driver uses `dev_err()` or `pr_err()`, the logs often identify the failing step.

---

# Step 3: Add debug messages

Instrument the `probe()` function to see how far execution gets.

```c
static int my_probe(...)
{
    pr_info("Probe started\n");

    pr_info("Allocating memory\n");

    pr_info("Mapping registers\n");

    pr_info("Registering char device\n");

    pr_info("Probe successful\n");

    return 0;
}
```

If the logs stop after "Mapping registers", you know the next operation is failing.

---

# Step 4: Check the return value

Never ignore the return value of kernel APIs.

```c
ret = request_irq(...);

if (ret) {
    pr_err("request_irq failed %d\n", ret);
    return ret;
}
```

Similarly check:

```c
ret = alloc_chrdev_region(...);
```

```c
ret = cdev_add(...);
```

```c
ret = class_create(...);
```

```c
ret = device_create(...);
```

Each failure should be logged.

---

# Step 5: Common failure points

| Function                | Possible reason                          |
| ----------------------- | ---------------------------------------- |
| `alloc_chrdev_region()` | Major/minor allocation failed            |
| `cdev_add()`            | Invalid `cdev` or duplicate registration |
| `class_create()`        | Class creation failed                    |
| `device_create()`       | Sysfs/device node creation failed        |
| `request_irq()`         | IRQ already in use or invalid            |
| `ioremap()`             | Register mapping failed                  |
| `clk_prepare_enable()`  | Clock unavailable                        |
| `gpio_request()`        | GPIO busy or invalid                     |
| `regulator_enable()`    | Power regulator unavailable              |

---

# Step 6: Verify device-tree or platform data

For platform drivers, make sure the device description matches what the driver expects.

Example Device Tree:

```dts
mydevice@10000000 {
    compatible = "vendor,mydevice";
    reg = <0x10000000 0x1000>;
    interrupts = <5>;
};
```

Driver:

```c
static const struct of_device_id my_match[] = {
    { .compatible = "vendor,mydevice" },
    { }
};
```

If the `compatible` strings don't match, `probe()` won't even be called.

---

# Step 7: Confirm the driver is bound

Check whether the driver is associated with the device.

```bash
ls /sys/bus/platform/drivers/
```

or

```bash
ls /sys/bus/i2c/drivers/
```

If the device isn't listed under the driver, binding or matching failed before `probe()`.

---

# Step 8: Verify device node creation

If `probe()` succeeds but `/dev/mydevice` is missing:

```bash
ls -l /dev/mydevice
```

Check whether:

```c
device_create(...)
```

was called successfully.

Also inspect:

```bash
ls /sys/class/
```

---

# Step 9: Enable dynamic debug (if supported)

If the driver uses `pr_debug()` or `dev_dbg()`:

```bash
echo 'file my_driver.c +p' > /sys/kernel/debug/dynamic_debug/control
```

This enables additional debug output without recompiling the kernel.

---

# Step 10: Check kernel configuration

Ensure required options are enabled:

```bash
zcat /proc/config.gz | grep MY_DRIVER
```

or

```bash
grep MY_DRIVER /boot/config-$(uname -r)
```

Also verify that dependencies (bus support, GPIO, IRQ framework, etc.) are enabled.

---

# Step 11: Use ftrace or tracepoints

To confirm whether `probe()` is entered:

```bash
echo function > /sys/kernel/debug/tracing/current_tracer
echo my_probe > /sys/kernel/debug/tracing/set_ftrace_filter
cat /sys/kernel/debug/tracing/trace
```

This can help distinguish between:

* `probe()` never being called.
* `probe()` being called and failing.

---

# Example of robust error handling

```c
static int my_probe(struct platform_device *pdev)
{
    int ret;

    dev_info(&pdev->dev, "Probe started\n");

    ret = alloc_chrdev_region(&devno, 0, 1, "mydev");
    if (ret) {
        dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    ret = cdev_add(&my_cdev, devno, 1);
    if (ret) {
        dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);
        unregister_chrdev_region(devno, 1);
        return ret;
    }

    dev_info(&pdev->dev, "Probe successful\n");
    return 0;
}
```

---

# A systematic debugging checklist

```text
Driver loaded?
      │
      ▼
Is probe() called?
      │
      ├── No
      │     ├── Device not detected
      │     ├── Driver not matched
      │     └── Wrong compatible/ID table
      │
      └── Yes
            │
            ▼
Which API fails?
            │
            ├── Memory allocation
            ├── Resource acquisition
            ├── IRQ request
            ├── Register mapping
            ├── cdev registration
            ├── class/device creation
            └── Hardware initialization
```

This step-by-step approach isolates whether the problem is in **device matching**, **resource acquisition**, **character device registration**, or **hardware initialization**, making it much easier to identify the root cause.


When a character driver fails in `probe()`, experienced kernel developers don't immediately look at the code. They first determine **where in the driver model the failure occurs**. The debugging process is systematic because the device goes through several stages before `probe()` is even called.

---

# Linux Driver Probe Lifecycle

```text
                    Hardware Exists
                          │
                          ▼
                 Kernel Detects Device
                          │
                          ▼
                  Bus Enumerates Device
             (PCI/I2C/SPI/Platform/USB)
                          │
                          ▼
                 Device Object Created
               (struct device initialized)
                          │
                          ▼
          Driver Registration (module_init)
                          │
                          ▼
          Driver Matching (compatible/ID table)
                          │
                Match Successful?
                  │             │
                No              Yes
                │                ▼
         probe() never      probe() called
            called               │
                                 ▼
                  Driver Initialization
      ┌────────────────────────────────────┐
      │ Allocate resources                 │
      │ Map registers                      │
      │ Enable clocks                      │
      │ Enable regulators                  │
      │ Configure GPIOs                    │
      │ Request IRQ                        │
      │ Initialize hardware                │
      │ Register char device               │
      │ Create /dev node                   │
      └────────────────────────────────────┘
                          │
                    Success or Failure
```

The first question is:

> **Did `probe()` run at all?**

Many debugging sessions stop here because the issue isn't inside `probe()`—it's that `probe()` was never called.

---

# Step 1: Verify the Driver Is Loaded

Check whether the module is loaded.

```bash
lsmod | grep mydriver
```

Example:

```text
mydriver            20480    0
```

If it isn't loaded:

```bash
insmod mydriver.ko
```

or

```bash
modprobe mydriver
```

If `insmod` itself fails:

```text
insmod: ERROR: could not insert module
```

look at:

```bash
dmesg
```

Possible reasons:

```
Unknown symbol
```

means

* Missing dependency
* Wrong kernel version
* Wrong exported symbol

Example:

```
mydriver: Unknown symbol gpio_request
```

Your kernel may not have GPIO support enabled, or the required module isn't loaded.

---

# Step 2: Did probe() Execute?

The easiest test:

```c
static int my_probe(...)
{
    pr_info("Probe entered\n");
    ...
}
```

Load the driver.

Now:

```bash
dmesg
```

If you never see

```
Probe entered
```

then `probe()` wasn't called.

That means the issue is **before the driver initialization**.

---

# Step 3: Why Was probe() Never Called?

Linux calls `probe()` only after matching a device to a driver.

For a platform driver:

```c
static const struct of_device_id my_ids[] = {
    {
        .compatible = "company,mydevice",
    },
    {}
};
```

Device Tree:

```dts
mydevice@40000000 {
    compatible = "company,mydevice";
};
```

If they differ:

Driver:

```
company,mydevice
```

Device Tree:

```
company,my_device
```

No match.

No probe.

---

# Verify Matching

Platform devices:

```bash
ls /sys/bus/platform/devices/
```

Drivers:

```bash
ls /sys/bus/platform/drivers/
```

If your device exists but isn't linked to the driver:

```
probe() never runs.
```

---

# Step 4: Check Device Tree

Verify that the kernel actually parsed the Device Tree.

Look at

```bash
ls /proc/device-tree/
```

Search for your node.

```bash
find /proc/device-tree -name "*mydevice*"
```

If missing:

* Wrong DTB
* DTB not loaded
* Node disabled

Example:

```dts
status = "disabled";
```

No probe.

Should be

```dts
status = "okay";
```

---

# Step 5: Inspect Every Resource

Most probe failures occur while requesting resources.

Example:

```c
res = platform_get_resource(pdev,
                            IORESOURCE_MEM,
                            0);
```

Possible failures:

```
NULL
```

means

* reg property missing

Device Tree should contain

```dts
reg = <0x40000000 0x1000>;
```

---

Next:

```c
base = devm_ioremap_resource(...);
```

Possible failure:

```
ERR_PTR(...)
```

Meaning

* address conflict
* invalid resource
* resource already owned

---

IRQ

```c
irq = platform_get_irq(pdev,0);
```

Could return

```
-ENXIO
```

Device Tree lacks

```dts
interrupts = <5>;
```

---

GPIO

```c
gpio = devm_gpiod_get(...);
```

Possible failures:

```
GPIO busy

GPIO missing

Invalid GPIO
```

---

Clock

```c
clk = devm_clk_get(...)
```

Possible errors

```
Clock provider absent

Clock disabled

Clock not described
```

---

Regulator

```c
regulator = devm_regulator_get(...)
```

Failure:

```
No power rail
```

---

# Step 6: Character Device Registration

Suppose hardware initialization succeeds.

Now

```c
alloc_chrdev_region()
```

Failure?

Possible reasons:

```
Memory

Duplicate allocation
```

Next

```c
cdev_add()
```

Failure:

```
Already registered

Bad major/minor
```

Then

```c
class_create()
```

Failure:

```
ENOMEM
```

Finally

```c
device_create()
```

Failure:

No

```
/dev/mydevice
```

---

# Step 7: Decode Error Codes

Kernel functions return negative errno values.

Common examples:

| Error           | Meaning                    |
| --------------- | -------------------------- |
| `-ENOMEM`       | Memory allocation failed   |
| `-EINVAL`       | Invalid argument           |
| `-ENODEV`       | Device not found           |
| `-ENXIO`        | Missing hardware resource  |
| `-EBUSY`        | Resource already in use    |
| `-EPROBE_DEFER` | Dependency isn't ready yet |
| `-EIO`          | Hardware I/O failure       |
| `-ETIMEDOUT`    | Hardware didn't respond    |

Never print only:

```c
return ret;
```

Instead:

```c
dev_err(dev,
        "request_irq failed %d\n",
        ret);
```

---

# Step 8: The Special Case of `-EPROBE_DEFER`

This is very common.

Suppose your driver needs

* regulator
* clock
* GPIO controller

but those drivers haven't initialized yet.

Then

```c
devm_clk_get()
```

returns

```
-EPROBE_DEFER
```

The kernel will retry your `probe()` later.

You can see this in logs.

```
probe defer
```

This isn't necessarily an error—it's part of the kernel's dependency handling.

---

# Step 9: Dynamic Debugging

If your driver uses:

```c
dev_dbg(dev,"Init GPIO\n");
```

enable it:

```bash
echo 'file mydriver.c +p' \
> /sys/kernel/debug/dynamic_debug/control
```

Now every `dev_dbg()` prints.

---

# Step 10: Use ftrace

Enable function tracing:

```bash
echo function > \
/sys/kernel/debug/tracing/current_tracer
```

Trace your probe:

```bash
echo my_probe > \
/sys/kernel/debug/tracing/set_ftrace_filter
```

Read:

```bash
cat /sys/kernel/debug/tracing/trace
```

You'll see something like:

```
my_probe()
 platform_get_resource()
 devm_ioremap_resource()
 request_irq()
 cdev_add()
 device_create()
```

If tracing stops after `request_irq()`, the failure likely occurred there.

---

# Step 11: Follow the Kernel's Error Path

Well-written drivers use cleanup labels so partially initialized resources are released correctly.

```c
static int my_probe(struct platform_device *pdev)
{
    int ret;

    ret = alloc_chrdev_region(&devno, 0, 1, "mydev");
    if (ret)
        return ret;

    ret = cdev_add(&my_cdev, devno, 1);
    if (ret)
        goto err_unregister;

    return 0;

err_unregister:
    unregister_chrdev_region(devno, 1);
    return ret;
}
```

If `cdev_add()` fails, the previously allocated device number is released before returning.

---

# A Senior Kernel Engineer's Debugging Flow

```text
Module inserted?
      │
      ├── No
      │      ↓
      │   Check insmod/modprobe errors
      │
      ▼
Device detected?
      │
      ├── No
      │      ↓
      │   Check hardware, Device Tree, bus
      │
      ▼
Driver matched?
      │
      ├── No
      │      ↓
      │   Check compatible strings / ID tables
      │
      ▼
probe() entered?
      │
      ├── No
      │      ↓
      │   Check bus registration and matching
      │
      ▼
Which initialization step fails?
      │
      ├── Memory allocation
      ├── I/O memory mapping
      ├── Clock/regulator/GPIO acquisition
      ├── IRQ request
      ├── Hardware initialization
      ├── Character device registration
      └── /dev node creation
      │
      ▼
Read the returned errno
      │
      ▼
Fix the failing dependency or initialization step
```

This structured approach helps isolate failures efficiently, from **device discovery and driver matching** down to **resource acquisition, hardware initialization, and character device registration**. It also prevents chasing the wrong problem—for example, debugging `request_irq()` when the real issue is that `probe()` was never invoked because the driver and device never matched.
