To support **multiple I2C devices using the same Linux driver**, your driver must be written **generically**, and the Linux I2C subsystem will instantiate a new `i2c_client` for each matching device node.

---

## ✅ What You’ll Learn

* How one driver handles **multiple devices** on same or different buses.
* What changes are needed in **Device Tree**, driver code, and logic.
* How to manage **per-device state** properly.

---

## 💡 Scenario

You want to use a single generic I2C driver (like your `my_rtc_driver.c`) for **multiple devices**, for example:

| Device Name | I2C Bus | I2C Addr | Use Case     |
| ----------- | ------- | -------- | ------------ |
| RTC1        | i2c1    | 0x68     | Timekeeping  |
| RTC2        | i2c1    | 0x69     | Backup RTC   |
| EEPROM      | i2c2    | 0x50     | Data storage |

---

## 🧾 Step 1: Device Tree with Multiple Devices

You declare each I2C device instance in the Device Tree under respective controller.

```dts
&i2c1 {
    rtc1: rtc@68 {
        compatible = "custom,my-rtc";
        reg = <0x68>;
        label = "rtc-primary";
    };

    rtc2: rtc@69 {
        compatible = "custom,my-rtc";
        reg = <0x69>;
        label = "rtc-backup";
    };
};

&i2c2 {
    eeprom@50 {
        compatible = "custom,my-rtc";  // Reuse same driver!
        reg = <0x50>;
        label = "eeprom-slot";
    };
};
```

---

## 🧠 Step 2: How Linux I2C Works for Multiple Devices

Each matching node creates a **separate `i2c_client`**.

Your driver’s `.probe()` is called once **per device**.

The Linux kernel matches:

* Device Tree `compatible = "custom,my-rtc"`
* Driver `.of_match_table`

---

## 🧠 Step 3: Driver Design for Multiple Devices

Your existing driver already supports this, as long as:

* You **don’t use global variables**
* Use `i2c_client` context to access device-specific info

```c
static int my_rtc_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    const char *label;

    // Read device-specific label from DT
    if (device_property_read_string(&client->dev, "label", &label) == 0)
        dev_info(&client->dev, "Device label: %s\n", label);

    dev_info(&client->dev, "Probing I2C device at address 0x%x\n", client->addr);

    // Read register
    int val = i2c_smbus_read_byte_data(client, 0x00);
    dev_info(&client->dev, "Reg[0x00] = 0x%02x\n", val);

    return 0;
}
```

You can store per-device state using `dev_set_drvdata()`:

```c
struct my_rtc_priv {
    int reg_base;
    const char *label;
};

struct my_rtc_priv *priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
priv->reg_base = 0x00;
priv->label = label;
i2c_set_clientdata(client, priv);
```

---

## 🔁 Step 4: How Kernel Calls Your Driver Multiple Times

Internally:

```c
for each i2c node in DT:
    if compatible matches driver:
        create i2c_client
        call probe(i2c_client)
```

So, if you have 3 devices with `compatible = "custom,my-rtc"`, your `probe()` is called **3 times**, each with a separate `i2c_client`.

---

## 📊 Example Logs (Simulated)

```log
[    0.512345] my_rtc_driver: Probing I2C device at 0x68
[    0.512389] my_rtc_driver: Device label: rtc-primary
[    0.512420] my_rtc_driver: Reg[0x00] = 0x41

[    0.513000] my_rtc_driver: Probing I2C device at 0x69
[    0.513045] my_rtc_driver: Device label: rtc-backup
[    0.513070] my_rtc_driver: Reg[0x00] = 0x43

[    0.514000] my_rtc_driver: Probing I2C device at 0x50
[    0.514045] my_rtc_driver: Device label: eeprom-slot
[    0.514070] my_rtc_driver: Reg[0x00] = 0xFE
```

---

## ✅ Summary

| Step          | Description                                                        |
| ------------- | ------------------------------------------------------------------ |
| Device Tree   | Define each I2C device with same `compatible`                      |
| Kernel Match  | Kernel creates separate `i2c_client` and calls `probe()` for each  |
| Driver Code   | Store per-device state in `priv`, use `i2c_client` correctly       |
| Shared Driver | As long as logic is generic, one driver can serve multiple devices |

---

## 🚀 Bonus

To further enhance multi-device support:

* Create a sysfs entry `/sys/class/i2c-rtc/rtc-primary/` etc.
* Use `platform_get_drvdata()` in remove/shutdown callbacks
* Create a **platform RTC class device** under `/dev/rtcX`

---

Would you like me to extend this into:

* A real DS1307/EEPROM multi-instance driver?
* A `sysfs`-based attribute export per I2C device?

Let me know!
