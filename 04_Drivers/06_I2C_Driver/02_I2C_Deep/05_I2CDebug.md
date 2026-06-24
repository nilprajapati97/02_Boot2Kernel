When a device **doesn’t show up in `i2cdetect`**, it usually indicates a **hardware, driver, or configuration issue**. Let’s go through a complete and practical **debug checklist** — from physical wiring to kernel configuration.

---

## 🧠 1. **Double-Check the Hardware Connection**

| Check                      | Details                                                                                  |
| -------------------------- | ---------------------------------------------------------------------------------------- |
| **VCC & GND**              | Is the device properly powered? Use a multimeter.                                        |
| **SDA/SCL connection**     | Are SDA/SCL lines connected to correct I2C pins on your SoC/board?                       |
| **Pull-up resistors**      | Are 4.7kΩ or 10kΩ pull-ups present on SDA and SCL?                                       |
| **Address selection pins** | If device has address pins (like A0/A1/A2), make sure they match the address you expect. |
| **Scope check (optional)** | Use an oscilloscope to check for activity on SCL/SDA while running `i2cdetect`.          |

---

## 🧪 2. **Verify I2C Controller is Enabled in Kernel**

### Check with:

```bash
ls /dev/i2c-*
```

If no `i2c-*` appears:

* Your I2C controller is **not enabled in device tree or kernel config**.

---

## 📜 3. **Check Device Tree or ACPI (for ARM/Linux boards)**

For **Device Tree-based boards** (like BeagleBone, iMX, STM32, Raspberry Pi, etc.):

Example node in `.dts`:

```dts
&i2c1 {
    status = "okay";
    eeprom@50 {
        compatible = "atmel,24c02";
        reg = <0x50>;
    };
};
```

Then compile and flash the DTB.

To verify loaded tree:

```bash
sudo cat /proc/device-tree/i2c@*/status
```

---

## 🔍 4. **Use `i2cdetect -l` to List I2C Buses**

```bash
i2cdetect -l
```

This tells you:

* How many I2C adapters are recognized by Linux.
* Their names (`i2c-0`, `i2c-1`, etc.).

You must probe the correct bus:

```bash
sudo i2cdetect -y <bus_number>
```

---

## 🔧 5. **Try Safe Probing Modes**

Some I2C devices (like power regulators or touchscreens) **don’t like writes**.

Try **read-only mode**:

```bash
sudo i2cdetect -y -r 1
```

Or **probe only safe addresses**:

```bash
sudo i2cdetect -y 1 0x20 0x2f
```

Avoid `-a` (don’t probe reserved 0x00–0x02 or 0x78–0x7f unless absolutely necessary).

---

## 📁 6. **Check Kernel Logs for Errors**

Run this **before and after** inserting or probing device:

```bash
dmesg | tail -n 50
```

Look for:

* “i2c timeout”
* “i2c transfer failed”
* “no acknowledge”
* "Device not responding"

---

## 👨‍🔧 7. **Use `i2c-tools` Manually**

Try:

```bash
sudo i2cdump -y 1 0x50
```

If this fails:

* Device may not be responding
* You may have a wrong address or bus
* Or pullups may be missing

---

## 🧩 8. **Try With Another Known Working Device**

To rule out wiring or bus issues:

* Connect a known I2C device (e.g., an MPU6050 or PCF8574)
* Check if that shows up in `i2cdetect`.

If nothing shows up → Bus or hardware issue
If the other device shows → Your original device may be:

* Broken
* Incorrectly addressed
* Needs special initialization

---

## 📡 9. **Bus Busy / Stuck Condition**

Sometimes SCL/SDA line gets stuck low:

* Caused by misbehaving slaves
* Power reset the device or board
* Or manually toggle GPIOs to free the lines

Use:

```bash
i2cdetect -y 1
```

If it hangs forever: SCL/SDA might be **stuck low**.

---

## 📦 10. **I2C Driver Conflicts or Claiming**

Some devices are not detected by `i2cdetect` because a **kernel driver already claimed the address**.

Check:

```bash
cat /sys/bus/i2c/devices/i2c-1/*/name
```

Or:

```bash
ls /sys/bus/i2c/devices/1-0050/
```

This means the device **is already loaded**, and `i2cdetect` won’t probe that address.

---

## ✅ Final Checklist Summary:

| Checkpoint                      | What to Confirm                              |
| ------------------------------- | -------------------------------------------- |
| `/dev/i2c-*` exists             | I2C controller is up                         |
| Correct device tree             | Bus and slave address mapped                 |
| Physical wiring                 | SDA/SCL connected, powered, pull-ups present |
| Try `-r` mode                   | Safer probe for read-sensitive devices       |
| Try address range               | `i2cdetect -y 1 0x20 0x2f`                   |
| Check if driver already claimed | Look in `/sys/bus/i2c/devices/`              |
| Check logs                      | `dmesg` output for I2C errors                |
| Probe another device            | Rule out board/I2C bus issue                 |

---

### Need help with a real-world log?

Send me the output of:

```bash
i2cdetect -l
i2cdetect -y -r 1
ls /dev/i2c*
dmesg | tail -n 50
```

Or describe your I2C device and platform (e.g., SDM660, Raspberry Pi, custom board), and I’ll help step-by-step.
