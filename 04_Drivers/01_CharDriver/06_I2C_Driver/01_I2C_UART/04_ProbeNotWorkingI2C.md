Let’s go step-by-step to debug the issue: **“My I2C driver’s `probe()` function is not being called”** — this is a very common issue when integrating an I2C device driver.

---

## 🧩 Scenario: Probe Not Called — Step-by-Step Debugging

---

### ✅ **Step 1: Confirm Driver is Built and Loaded**

#### 1.1 Static Kernel Build:

If driver is built into the kernel (`obj-y +=`):

```bash
dmesg | grep mytempsensor
```

Or check in `/boot/config-*`:

```bash
grep MYTEMPSENSOR /boot/config-$(uname -r)
```

#### 1.2 Dynamic Module:

```bash
modinfo mytempsensor.ko
insmod mytempsensor.ko
dmesg | tail
```

If you don’t see “Probing device…” → probe not called.

---

### ✅ **Step 2: Match `compatible` String**

#### 2.1 Check Your Driver `i2c_driver` Structure

```c
static const struct of_device_id mytemp_dt_ids[] = {
  { .compatible = "myvendor,mytempsensor" },
  { }
};
MODULE_DEVICE_TABLE(of, mytemp_dt_ids);

static struct i2c_driver mytemp_driver = {
  .driver = {
    .name = "mytempsensor",
    .of_match_table = mytemp_dt_ids,
  },
  .probe = mytemp_probe,
  .remove = mytemp_remove,
};
```

#### 2.2 Check Your Device Tree

```dts
&i2c1 {
    mytempsensor@48 {
        compatible = "myvendor,mytempsensor";
        reg = <0x48>;
    };
};
```

✅ **Checklist:**

* `compatible` string matches exactly: `"myvendor,mytempsensor"`
* `reg` is correct I2C address (0x48)
* Device is under correct I2C controller node

---

### ✅ **Step 3: Verify Device Node Creation**

```bash
ls /sys/bus/i2c/devices/
```

Expected:

```
1-0048/
```

If missing → likely DT not parsed correctly.

---

### ✅ **Step 4: Check Driver Registration**

```bash
cat /sys/bus/i2c/drivers/
```

Your driver should be listed:

```
mytempsensor/
```

If not:

* `driver_register()` or `i2c_add_driver()` might not be called
* Module might have failed to load (check `dmesg`)

---

### ✅ **Step 5: Dump Device Tree at Runtime**

```bash
cat /proc/device-tree/soc/i2c@.../mytempsensor@48/compatible
```

It should return:

```
myvendor,mytempsensor
```

If not found:

* Device tree didn’t compile correctly
* DT overlay might not be applied
* Wrong I2C path or address

---

### ✅ **Step 6: Force Match (Debug Only)**

To test matching manually:

```bash
echo mytempsensor 0x48 > /sys/bus/i2c/devices/i2c-1/new_device
```

This forces driver match. Check logs:

```bash
dmesg | tail
```

🛑 **Warning:** Only for debugging — don’t use in production.

---

### ✅ **Step 7: Add Logs to `i2c_driver` Registration**

To confirm it’s being registered:

```c
printk(KERN_INFO "mytempsensor: registering i2c driver\n");
```

And inside `probe()`:

```c
dev_info(&client->dev, "probe called\n");
```

---

### ✅ **Step 8: Check Kernel Version Mismatch**

If you are building modules separately, ensure:

* Your kernel headers match running kernel (`uname -r`)
* Driver uses correct `MODULE_DEVICE_TABLE()`

---

### ✅ **Step 9: Look for `i2c-core` Errors**

```bash
dmesg | grep i2c
```

You might see:

```
i2c-core: driver mytempsensor didn't bind to device 1-0048
```

→ Suggests binding issue

---

### ✅ **Step 10: Confirm Kernel Probing Framework Works**

Sometimes the issue lies in I2C core not invoking drivers:

* Try probing with a known working I2C driver (e.g. `rtc-ds1307`)
* Swap your driver’s `compatible` string to match a known working one — see if `probe()` gets called

---

## 🧠 Summary of Fix Path

| Issue                                 | Fix                                            |
| ------------------------------------- | ---------------------------------------------- |
| `probe()` not called                  | `compatible` string mismatch                   |
| Device not in `/sys/bus/i2c/devices/` | Invalid DT or bus address                      |
| Driver not in `/sys/bus/i2c/drivers/` | Module failed or not loaded                    |
| No `dmesg` logs                       | Add `printk`, check log level                  |
| Force probe test                      | Write to `/sys/bus/i2c/devices/.../new_device` |

---

Would you like a **real working I2C driver + DTS overlay + logs** to simulate this full debug flow on a real board (e.g., Raspberry Pi or BeagleBone)?
