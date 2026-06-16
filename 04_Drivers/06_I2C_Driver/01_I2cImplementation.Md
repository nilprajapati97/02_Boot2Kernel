Writing a **generic I2C driver from scratch in Linux** for a custom I2C slave device like an **RTC chip** involves multiple layers. Below is a **complete in-depth explanation and example** from scratch: device tree to driver to test application.

---

## ✅ Goal

You got a new I2C device (e.g., **RTC chip like DS1307 or a custom I2C EEPROM**). You want to write a **generic Linux I2C client driver** for it **from scratch**, get data over I2C, and verify it works.

---

## 🔁 I2C Driver Layers in Linux

```
+-----------------------------+
|  User-space App (I2C ioctl)|
+-----------------------------+
|   /dev/i2c-X (I2C-dev node)|
+-----------------------------+
|    I2C Client Driver (You) |
+-----------------------------+
|    I2C Bus Adapter (HW IP) |
+-----------------------------+
|        I2C Controller      |
+-----------------------------+
|       Hardware (I2C Chip)  |
+-----------------------------+
```

---

## 📦 Steps to Write a Generic I2C Client Driver

---

### 🧾 1. **Device Tree Node for Your I2C Device**

Add this under your I2C controller node (`i2c@xxxx`) in the device tree:

```dts
&i2c1 {
    status = "okay";

    rtc@68 {
        compatible = "custom,my-rtc";     // Your driver will match this
        reg = <0x68>;                     // I2C address
    };
};
```

---

### 📂 2. **Directory & Kconfig**

In your driver folder (e.g., `drivers/i2c/custom/`):

**Kconfig**

```kconfig
config I2C_MY_RTC
    tristate "Custom I2C RTC Driver"
    depends on I2C
    help
      A simple I2C client driver for RTC or any I2C chip
```

**Makefile**

```makefile
obj-$(CONFIG_I2C_MY_RTC) += my_rtc_driver.o
```

---

### 🧠 3. **Core Driver Code (`my_rtc_driver.c`)**

```c
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>

#define RTC_REG_SECONDS   0x00  // Example register offset

static int my_rtc_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    u8 val;
    int ret;

    dev_info(&client->dev, "Probing I2C RTC device\n");

    // Read 1 byte from register 0x00 (seconds register)
    ret = i2c_smbus_read_byte_data(client, RTC_REG_SECONDS);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read from device\n");
        return ret;
    }

    val = ret;
    dev_info(&client->dev, "RTC Seconds Register: 0x%02x\n", val);

    return 0;
}

static int my_rtc_remove(struct i2c_client *client)
{
    dev_info(&client->dev, "Removing RTC driver\n");
    return 0;
}

static const struct of_device_id my_rtc_of_match[] = {
    { .compatible = "custom,my-rtc", },
    { }
};
MODULE_DEVICE_TABLE(of, my_rtc_of_match);

static const struct i2c_device_id my_rtc_id[] = {
    { "my_rtc", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, my_rtc_id);

static struct i2c_driver my_rtc_driver = {
    .driver = {
        .name = "my_rtc_driver",
        .of_match_table = my_rtc_of_match,
    },
    .probe = my_rtc_probe,
    .remove = my_rtc_remove,
    .id_table = my_rtc_id,
};

module_i2c_driver(my_rtc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Generic I2C Client Driver for RTC or custom device");
```

---

### 🔧 4. **Kernel Config + Build**

In `menuconfig`:

```bash
Device Drivers → I2C support → Custom I2C RTC Driver → [*]
```

Then compile kernel or module.

---

### 🔌 5. **Load and Test**

```bash
dmesg | grep rtc
# Should see: "RTC Seconds Register: 0xXX"
```

You can add more registers or write API:

```c
// Write to device register
i2c_smbus_write_byte_data(client, reg_addr, value);
```

---

## 🧪 6. **User-Space Access Example**

If your driver supports `i2c-dev`, you can also access via `/dev/i2c-1`:

```c
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/i2c-1", O_RDWR);
    ioctl(fd, I2C_SLAVE, 0x68); // Address of your device

    char reg = 0x00;
    write(fd, &reg, 1);

    char val;
    read(fd, &val, 1);

    printf("Seconds: %02x\n", val);
    close(fd);
}
```

---

## 🧠 Key Concepts

| Concept                    | Meaning                                  |
| -------------------------- | ---------------------------------------- |
| `i2c_client`               | Represents your connected slave device   |
| `i2c_smbus_read_byte_data` | Reads 1 byte from specific register      |
| `of_device_id`             | Matches device tree node                 |
| `probe()`                  | Called when kernel finds matching device |

---

## 🧰 Debug Tips

* `dmesg` for errors (`Failed to probe`, `No ACK`, etc.)
* Use `i2cdetect` and `i2cdump` to verify connection:

  ```bash
  i2cdetect -y 1
  i2cdump -y 1 0x68
  ```

---

## ✅ Summary

| Step            | Action                                           |
| --------------- | ------------------------------------------------ |
| Device Tree     | Add I2C node with `compatible = "custom,my-rtc"` |
| Driver Code     | Match `.of_match_table` and use I2C APIs         |
| Register Access | Use `i2c_smbus_read_byte_data` / write           |
| Test            | See logs or use user-space app                   |

---

Would you like:

* A working example on DS1307 RTC?
* How to expose your driver data to sysfs (`/sys`) or `hwclock`?
* How to convert this to a full Linux RTC framework driver?

Let me know!
