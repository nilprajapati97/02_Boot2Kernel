Let's walk through a **real, full GPIO + I2C sensor platform driver example**, including:

* ✅ Device Tree
* ✅ Platform Driver
* ✅ Probe Flow
* ✅ I2C + GPIO read/write
* ✅ Logs + Testing

We’ll use a **fake temperature sensor** connected via **I2C** and **GPIO (interrupt)** — a very common embedded case (like Bosch BME280 or TMP102).

---

# ✅ Sensor Scenario

* I2C device address: `0x48` (common for TMP102)
* GPIO line used for Data Ready interrupt
* We’ll write a **Linux Platform Driver** for this

---

## 📘 1. Device Tree Node

```dts
i2c@78b6000 {
    status = "okay";
    mytempsensor@48 {
        compatible = "myvendor,mytempsensor";
        reg = <0x48>;                   // I2C address
        interrupt-parent = <&tlmm>;
        interrupts = <123 IRQ_TYPE_EDGE_FALLING>;  // GPIO interrupt line
        irq-gpios = <&tlmm 123 GPIO_ACTIVE_LOW>;
    };
};
```

* `reg = <0x48>` → I2C address
* `irq-gpios` → GPIO line connected to INT pin of sensor

---

## 📘 2. Driver Code (`mytempsensor.c`)

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

struct mytemp_dev {
    struct i2c_client *client;
    struct gpio_desc *irq_gpio;
    int irq;
};

static irqreturn_t mytemp_irq_handler(int irq, void *dev_id)
{
    struct mytemp_dev *dev = dev_id;
    u8 buf[2];
    int ret;

    ret = i2c_master_recv(dev->client, buf, 2);
    if (ret < 0) {
        pr_err("I2C read failed\n");
        return IRQ_HANDLED;
    }

    int temp = (buf[0] << 8 | buf[1]) >> 4;
    pr_info("Temp = %d C\n", temp);

    return IRQ_HANDLED;
}

static int mytemp_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct mytemp_dev *dev;
    int ret;

    dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->client = client;

    // GPIO from DT
    dev->irq_gpio = devm_gpiod_get(&client->dev, "irq", GPIOD_IN);
    if (IS_ERR(dev->irq_gpio))
        return PTR_ERR(dev->irq_gpio);

    dev->irq = gpiod_to_irq(dev->irq_gpio);
    if (dev->irq < 0)
        return dev->irq;

    // Request IRQ
    ret = devm_request_threaded_irq(&client->dev, dev->irq,
        NULL, mytemp_irq_handler,
        IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
        "mytemp_irq", dev);

    if (ret) {
        dev_err(&client->dev, "Failed to request IRQ\n");
        return ret;
    }

    i2c_set_clientdata(client, dev);
    dev_info(&client->dev, "MyTemp sensor probed successfully\n");
    return 0;
}

static int mytemp_remove(struct i2c_client *client)
{
    dev_info(&client->dev, "MyTemp sensor removed\n");
    return 0;
}

static const struct of_device_id mytemp_of_match[] = {
    { .compatible = "myvendor,mytempsensor", },
    {}
};
MODULE_DEVICE_TABLE(of, mytemp_of_match);

static const struct i2c_device_id mytemp_id[] = {
    { "mytempsensor", 0 },
    {}
};
MODULE_DEVICE_TABLE(i2c, mytemp_id);

static struct i2c_driver mytemp_driver = {
    .driver = {
        .name = "mytempsensor",
        .of_match_table = mytemp_of_match,
    },
    .probe = mytemp_probe,
    .remove = mytemp_remove,
    .id_table = mytemp_id,
};

module_i2c_driver(mytemp_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anil Example");
MODULE_DESCRIPTION("Fake Temp Sensor over I2C with GPIO IRQ");
```

---

## 🧩 3. Kconfig and Makefile

### `Kconfig`:

```kconfig
config MYTEMP_SENSOR
    tristate "My Temp Sensor Driver"
    depends on I2C && GPIOLIB && OF
    default y
```

### `Makefile`:

```makefile
obj-$(CONFIG_MYTEMP_SENSOR) += mytempsensor.o
```

---

## 🧪 4. Testing Steps

### Step 1: Check dmesg logs

```bash
dmesg | grep mytemp
```

Expected:

```
mytempsensor 0-0048: MyTemp sensor probed successfully
```

### Step 2: Simulate interrupt

* Use GPIO simulator or hardware pulse on INT pin
* Or use `echo 0 > /sys/class/gpio/gpio123/value`

Expected:

```
Temp = 27 C
```

---

## 🧠 5. How Matching Works (Deep)

1. **I2C core** scans `.dts` for `i2c@xxxx` nodes.
2. Finds `reg = <0x48>` → creates I2C client
3. Matches `.compatible = "myvendor,mytempsensor"` with driver's `of_match_table`
4. Calls → `probe()`
5. Reads `irq-gpios`, converts to IRQ, installs `IRQ handler`

---

## 🔄 6. Clean Unbind

To remove driver:

```bash
rmmod mytempsensor
```

---

## ✅ Summary Flow (Block)

```
[Device Tree Node]
     │
     ▼
[i2c-core] creates client for addr=0x48
     │
     ▼
[i2c_driver] mytempsensor matches compatible
     │
     ├── Probe()
     │    ├── Read irq-gpio
     │    ├── Request IRQ
     │    └── Store client data
     │
     ▼
Interrupt → IRQ handler → I2C read → Print Temp
```

---

## 📦 Real Sensor Equivalents

| Sensor   | Protocol | GPIO IRQ | Real Driver                        |
| -------- | -------- | -------- | ---------------------------------- |
| TMP102   | I2C      | Optional | `drivers/hwmon/tmp102.c`           |
| BME280   | I2C/SPI  | Optional | `drivers/iio/pressure/bmp280.c`    |
| APDS9960 | I2C      | Yes      | `drivers/iio/proximity/apds9960.c` |

---

Would you like:

* A real **GPIO button** platform driver example?
* Or a **BME280** / **TMP102** complete driver for integration?

Let me know your use-case (sensor/device), and I’ll tailor a full working flow.
