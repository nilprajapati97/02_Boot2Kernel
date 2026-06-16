Once you implement and integrate your custom I2C driver in the Linux kernel, debugging is a critical step to ensure it's working correctly. Here's a **step-by-step guide to debugging an I2C driver in Linux**, starting from basic checks to advanced kernel-level debugging.

---

## 🧩 Step-by-Step Debugging Plan for I2C Driver

---

### ✅ **Step 1: Validate Device Tree Integration**

#### 🔍 Check if I2C node is created by the kernel

```bash
dmesg | grep i2c
```

Look for:

```
i2c i2c-1: Added multiplexed i2c bus 1
```

#### 📁 Verify device node in sysfs

```bash
ls /sys/bus/i2c/devices/
```

You should see something like:

```
1-0048/               (bus 1, address 0x48)
```

---

### ✅ **Step 2: Confirm Probe Function is Called**

Ensure your probe is triggered.

```bash
dmesg | grep mytempsensor
```

Expected output:

```
[ 2.123456 ] mytempsensor 1-0048: Temp sensor probed at 0x48
```

#### 🔧 If `probe()` is NOT called:

* Check `compatible` string in DTS and driver matches
* Confirm driver is registered in kernel (`modinfo mytempsensor.ko`)
* Recheck I2C bus and address (`reg = <0x48>`)

---

### ✅ **Step 3: Enable Debug Print in Driver**

Use `dev_info`, `dev_dbg`, `dev_err`, etc.

```c
dev_info(&client->dev, "Probing device\n");
dev_err(&client->dev, "Failed to read register\n");
```

Then in kernel boot args (`/boot/extlinux.conf` or `grub.cfg`):

```bash
loglevel=8
```

OR enable dynamic debug:

```bash
echo 'file mytempsensor.c +p' > /sys/kernel/debug/dynamic_debug/control
```

---

### ✅ **Step 4: Check Kernel I2C Bus Communication**

#### 📦 Use `i2cdetect` to scan the I2C bus:

```bash
i2cdetect -y 1
```

Expected output:

```
     0 1 2 3 4 5 6 7 8 9 a b c d e f
00:          -- -- -- -- -- -- -- --
10: -- -- -- -- -- -- -- -- -- -- -- --
40: -- -- -- -- -- -- -- -- 48 -- -- --
```

If address 0x48 shows up, your sensor/device is connected.

---

### ✅ **Step 5: Use `i2cget` and `i2cset` to Read/Write**

```bash
# Read register 0x00
i2cget -y 1 0x48 0x00

# Write value 0x01 to register 0x01
i2cset -y 1 0x48 0x01 0x01
```

This helps isolate if the problem is in driver or hardware.

---

### ✅ **Step 6: Add Instrumentation Inside Driver**

Edit your probe/read/write handlers:

```c
dev_info(&client->dev, "Reading register 0x00\n");

int ret = i2c_smbus_read_byte_data(client, 0x00);
if (ret < 0)
    dev_err(&client->dev, "Read failed with code %d\n", ret);
else
    dev_info(&client->dev, "Read value: 0x%x\n", ret);
```

---

### ✅ **Step 7: Use `trace_printk()` for Deeper Tracing**

Inside your driver:

```c
#include <linux/tracepoint.h>

trace_printk("Reached here: probe()\n");
```

View logs:

```bash
cat /sys/kernel/debug/tracing/trace
```

---

### ✅ **Step 8: Use I2C Core Logging**

Enable I2C core debug:

```bash
echo Y > /sys/module/i2c_core/parameters/debug
```

This shows I2C transactions happening in the kernel log.

---

### ✅ **Step 9: Use Logic Analyzer or Oscilloscope**

* If nothing works, verify electrical lines (SCL, SDA)
* Use **logic analyzer** to capture I2C transaction on the bus
* Check ACK/NACK, start/stop conditions, register values

---

### ✅ **Step 10: Use GDB + KGDB (Advanced)**

1. Enable KGDB in kernel
2. Set breakpoints in `probe()` or `read()` functions
3. Connect via serial to debug live

---

## 🧠 Summary Table

| Step | Tool or Technique               | Purpose                    |
| ---- | ------------------------------- | -------------------------- |
| 1    | `dmesg`, `sysfs`                | Confirm DT node and probe  |
| 2    | `dev_dbg`, `dev_err`            | Add logs to trace logic    |
| 3    | `i2cdetect`, `i2cget`           | Bus/device check, sanity   |
| 4    | `trace_printk`, `dynamic_debug` | Fine-grained debug         |
| 5    | `oscilloscope`, logic analyzer  | Hardware validation        |
| 6    | `KGDB`, `GDB`                   | Low-level driver debugging |

---

Would you like me to walk you through one debugging case step-by-step (e.g., "probe not being called")?
