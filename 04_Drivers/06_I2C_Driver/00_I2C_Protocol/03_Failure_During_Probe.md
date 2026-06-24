# Debugging I2C Failure During `probe()`

This is one of the most common Linux Kernel interview questions:

> **"Your I2C driver's probe() is failing. How do you debug it?"**

To answer properly, you must understand **everything that happens before `probe()` and inside `probe()`**.

---

# 1. Complete Flow Before `probe()`

Suppose Device Tree contains:

```dts
i2c0: i2c@12340000 {
        status = "okay";

        temp@48 {
                compatible = "ti,tmp102";
                reg = <0x48>;
        };
};
```

Boot flow:

```text
Kernel Boot
    |
    v
I2C Controller Probe
    |
    v
i2c_add_adapter()
    |
    v
Device Tree Parsing
    |
    v
i2c_client Created
    |
    v
i2c_driver Registered
    |
    v
i2c_device_match()
    |
    v
tmp102_probe()
```

If `probe()` is failing, determine:

```text
1. probe() not called?
2. probe() called but returned error?
3. probe() successful but device not working?
```

---

# Case 1: `probe()` Never Called

## Verify Driver Registration

Check:

```c
module_i2c_driver(tmp102_driver);
```

or

```c
i2c_add_driver(&tmp102_driver);
```

Add log:

```c
static int __init my_init(void)
{
        pr_info("Driver loaded\n");
        return i2c_add_driver(&my_driver);
}
```

Check:

```bash
dmesg | grep Driver
```

---

## Verify Compatible String

Driver:

```c
static const struct of_device_id tmp102_of_match[] = {
        { .compatible = "ti,tmp102" },
};
```

DT:

```dts
compatible = "ti,tmp102";
```

If DT says:

```dts
compatible = "ti,tmp101";
```

Then:

```text
probe() never called
```

---

## Verify Device Exists

Check:

```bash
ls /sys/bus/i2c/devices/
```

Expected:

```text
0-0048
```

No device means:

```text
i2c_client never created
```

---

# Case 2: `probe()` Called But Returns Error

Example:

```c
static int tmp102_probe(struct i2c_client *client)
{
        return -ENODEV;
}
```

Kernel:

```text
probe failed with error -19
```

Check:

```bash
dmesg | grep probe
```

---

# Step-by-Step Debug Inside Probe

Add logs:

```c
static int tmp102_probe(struct i2c_client *client)
{
        pr_info("Probe Start\n");

        ...
}
```

Find exact failure point.

---

# Case 3: I2C Communication Fails in Probe

Most probes perform:

```c
i2c_smbus_read_byte_data()
```

or

```c
i2c_transfer()
```

Example:

```c
ret = i2c_smbus_read_byte_data(client, CHIP_ID);
```

---

Flow:

```text
probe()
   |
   v
i2c_smbus_read_byte_data()
   |
   v
i2c_transfer()
   |
   v
Adapter Driver
   |
   v
Hardware Controller
```

---

# Failure: No ACK

Waveform:

```text
START

0x48

NACK
```

Logic Analyzer:

```text
SDA
Address Sent
No ACK
```

Probe returns:

```text
-EREMOTEIO
```

or

```text
-ENXIO
```

---

Possible Causes

### Device Not Powered

Measure:

```text
VDD
```

Expected:

```text
1.8V / 3.3V
```

---

### Wrong Address

DT:

```dts
reg = <0x49>;
```

Hardware:

```text
0x48
```

Probe fails.

---

### Device Held in Reset

Reset GPIO not released.

Example:

```c
gpiod_set_value(reset_gpio, 1);
```

missing.

---

# Case 4: Read Chip ID Fails

Many drivers do:

```c
id = i2c_smbus_read_byte_data(client, WHO_AM_I);
```

Example:

```c
0x0F -> WHO_AM_I
```

Expected:

```text
0x42
```

Actual:

```text
0x00
```

Probe returns:

```c
return -ENODEV;
```

---

Debug:

Capture waveform.

Expected:

```text
START

ADDR+W

ACK

0x0F

ACK

RESTART

ADDR+R

ACK

0x42

STOP
```

---

# Case 5: Regulator Failure

Probe:

```c
vdd = devm_regulator_get(dev, "vdd");
```

Fails:

```text
-EPROBE_DEFER
```

or

```text
-ENODEV
```

Kernel:

```text
Failed to get regulator
```

---

Check:

```bash
cat /sys/kernel/debug/regulator/regulator_summary
```

---

# Case 6: Clock Failure

Probe:

```c
clk = devm_clk_get(dev, NULL);
```

Fails:

```text
-ENOENT
```

or

```text
-EPROBE_DEFER
```

---

DT:

```dts
clocks = <&gcc GCC_I2C_CLK>;
```

Verify clock provider.

---

# Case 7: GPIO Failure

Probe:

```c
reset_gpio =
devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
```

Fails:

```text
-ENOENT
```

---

Check DT:

```dts
reset-gpios = <&gpio1 10 GPIO_ACTIVE_LOW>;
```

---

# Case 8: Interrupt Failure

Probe:

```c
irq = platform_get_irq(pdev, 0);
```

returns:

```text
-ENXIO
```

No interrupt configured.

---

Check:

```dts
interrupts = <45>;
```

---

# Case 9: Memory Allocation Failure

Probe:

```c
data = devm_kzalloc(...)
```

Rare but possible.

Returns:

```text
NULL
```

Then:

```c
return -ENOMEM;
```

---

# Case 10: Deferred Probe

Very common.

Probe:

```c
regulator_get()
```

returns:

```text
-EPROBE_DEFER
```

Kernel:

```text
deferred probe pending
```

---

Flow:

```text
Sensor Probe
     |
Regulator Missing
     |
-EPROBE_DEFER
     |
Kernel Retries Later
```

---

Check:

```bash
dmesg | grep defer
```

---

# Case 11: Probe Hangs

Example:

```c
while (!(status & READY))
        ;
```

Hardware never ready.

CPU stuck.

---

Debug:

```bash
echo w > /proc/sysrq-trigger
```

Stack trace:

```text
tmp102_probe()
 waiting forever
```

---

# Case 12: Bus Busy During Probe

Controller Status:

```text
BUS_BUSY = 1
```

Waveform:

```text
SCL HIGH

SDA LOW
```

Slave holding SDA.

Probe:

```text
timeout
```

---

Recover Bus

Generate:

```text
9 clocks
```

on SCL.

Release SDA.

---

# Case 13: Probe Returns `-EREMOTEIO`

Most common interview question.

Internally:

```text
Address Sent
No ACK
```

Controller sets:

```text
TX_ABRT
```

Driver returns:

```text
-EREMOTEIO
```

Meaning:

```text
Physical device didn't acknowledge
```

Check:

* Power
* Address
* Wiring
* Pull-ups
* Reset

---

# Advanced Debugging Commands

## Dynamic Debug

```bash
echo 'file drivers/i2c/* +p' \
> /sys/kernel/debug/dynamic_debug/control
```

---

## Trace I2C

```bash
trace-cmd record -e i2c
```

---

## Check Device

```bash
i2cdetect -y 0
```

---

## Interrupt Count

```bash
cat /proc/interrupts
```

---

## GPIO State

```bash
cat /sys/kernel/debug/gpio
```

---

## Pinctrl

```bash
cat /sys/kernel/debug/pinctrl/*/pinmux-pins
```

---

# Interview-Level Answer

If an interviewer asks:

> "Your I2C driver's `probe()` is failing. What is your debugging approach?"

A strong answer is:

```text
1. Verify probe() is actually called.
2. Verify DT compatible matching.
3. Verify i2c_client creation.
4. Add logs to identify exact failure point.
5. Check regulator, clock, reset, GPIO resources.
6. Verify I2C transactions using logic analyzer.
7. Check ACK/NACK behavior.
8. Verify interrupt functionality.
9. Check for -EPROBE_DEFER.
10. Inspect controller status registers.
11. Verify SDA/SCL idle levels and pull-ups.
12. Confirm chip ID read succeeds.
13. Use dynamic debug and tracepoints.
14. Check for bus busy/stuck conditions.
15. Analyze the returned error code (-ENODEV, -EREMOTEIO, -EPROBE_DEFER, -ETIMEDOUT).
```

This is the level of debugging expected from an experienced Linux Device Driver engineer working on I2C sensor, PMIC, touch, camera, or EEPROM drivers.
