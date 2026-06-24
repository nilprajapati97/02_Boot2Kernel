// SPDX-License-Identifier: GPL-2.0
/*
 * 02_i2c_client_driver.c
 *
 * A complete, heavily-annotated I2C CLIENT (device) driver skeleton for a
 * fictional register-based chip "FOO" (imagine a temperature sensor / EEPROM).
 *
 * This is the layer most people mean when they say "an I2C driver": it drives
 * ONE type of chip and talks to it through the I2C core. It does NOT know
 * anything about the SoC's I2C controller hardware -- that is the adapter
 * driver's job (see 03_i2c_adapter_driver.c).
 *
 * Build (against kernel headers):
 *   obj-m += 02_i2c_client_driver.o
 *   make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
 *
 * Flow recap:
 *   module load -> i2c_add_driver() -> core matches client -> .probe()
 *   -> verify chip -> register sysfs attrs -> transfers via i2c_transfer/smbus
 *   -> .remove() on unbind.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

/* ------------------------------------------------------------------ */
/* Chip register map (made up for the example)                        */
/* ------------------------------------------------------------------ */
#define FOO_REG_WHOAMI   0x0F   /* read-only chip-id register          */
#define FOO_REG_CONFIG   0x01   /* configuration                       */
#define FOO_REG_DATA     0x00   /* 16-bit measurement (big-endian)     */
#define FOO_CHIP_ID      0xA5   /* value WHOAMI must return            */

/* ------------------------------------------------------------------ */
/* Per-device private state. One instance per physical chip.          */
/* Allocated with devm_* so it is freed automatically on remove.      */
/* ------------------------------------------------------------------ */
struct foo_data {
	struct i2c_client *client;
	struct mutex       lock;   /* serialize multi-step register access */
	u8                 config; /* cached config shadow                 */
};

/* ================================================================== */
/* Low-level register helpers                                          */
/* ================================================================== */

/*
 * Read one 8-bit register using the SMBus helper. Works on every adapter
 * because the core emulates SMBus over raw I2C when needed.
 * Returns value (0..255) or negative errno.
 */
static int foo_read_reg8(struct foo_data *foo, u8 reg)
{
	return i2c_smbus_read_byte_data(foo->client, reg);
}

/* Write one 8-bit register. Returns 0 or negative errno. */
static int foo_write_reg8(struct foo_data *foo, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(foo->client, reg, val);
}

/*
 * Read a 16-bit big-endian measurement using a RAW I2C two-message
 * transaction: first write the register pointer, then a REPEATED START
 * and read 2 bytes. This is the canonical i2c_transfer() pattern and is
 * the thing interviewers most often ask you to write on a whiteboard.
 */
static int foo_read_data16(struct foo_data *foo, u16 *out)
{
	struct i2c_client *c = foo->client;
	u8 reg = FOO_REG_DATA;
	u8 buf[2];
	struct i2c_msg msgs[2] = {
		{	/* segment 1: write register pointer (no STOP after) */
			.addr  = c->addr,
			.flags = 0,            /* write                       */
			.len   = 1,
			.buf   = &reg,
		},
		{	/* segment 2: repeated START, then read 2 data bytes  */
			.addr  = c->addr,
			.flags = I2C_M_RD,     /* read                        */
			.len   = sizeof(buf),
			.buf   = buf,
		},
	};
	int ret;

	ret = i2c_transfer(c->adapter, msgs, 2);
	if (ret < 0)
		return ret;          /* bus / NACK error                 */
	if (ret != 2)
		return -EIO;         /* short transfer                   */

	*out = (buf[0] << 8) | buf[1];   /* big-endian assemble        */
	return 0;
}

/* ================================================================== */
/* sysfs interface: /sys/bus/i2c/devices/<bus>-<addr>/measurement      */
/* ================================================================== */

static ssize_t measurement_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct foo_data *foo = dev_get_drvdata(dev);
	u16 raw;
	int ret;

	mutex_lock(&foo->lock);
	ret = foo_read_data16(foo, &raw);
	mutex_unlock(&foo->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", raw);
}
static DEVICE_ATTR_RO(measurement);

static struct attribute *foo_attrs[] = {
	&dev_attr_measurement.attr,
	NULL,
};
ATTRIBUTE_GROUPS(foo);

/* ================================================================== */
/* probe / remove                                                      */
/* ================================================================== */

static int foo_probe(struct i2c_client *client)
{
	struct foo_data *foo;
	int id;

	/*
	 * 1. Make sure the adapter can do what we need. If we relied purely
	 *    on SMBus byte ops we would check I2C_FUNC_SMBUS_BYTE_DATA; since
	 *    we also do a raw i2c_transfer, require I2C_FUNC_I2C too.
	 */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "adapter lacks required functionality\n");
		return -EOPNOTSUPP;
	}

	/* 2. Probe the chip: read its WHO_AM_I and verify the identity. */
	id = i2c_smbus_read_byte_data(client, FOO_REG_WHOAMI);
	if (id < 0) {
		dev_err(&client->dev, "failed to read chip id: %d\n", id);
		return id;                 /* propagate bus error          */
	}
	if (id != FOO_CHIP_ID) {
		dev_err(&client->dev, "unexpected chip id 0x%02x\n", id);
		return -ENODEV;            /* not our chip                 */
	}

	/* 3. Allocate per-device state (auto-freed on remove). */
	foo = devm_kzalloc(&client->dev, sizeof(*foo), GFP_KERNEL);
	if (!foo)
		return -ENOMEM;

	foo->client = client;
	mutex_init(&foo->lock);
	i2c_set_clientdata(client, foo);   /* stash for remove/other cbs */

	/* 4. Bring the chip into a known state. */
	foo->config = 0x00;
	if (foo_write_reg8(foo, FOO_REG_CONFIG, foo->config))
		dev_warn(&client->dev, "could not write config\n");

	/* 5. Expose sysfs attributes (here, manually; or use .dev_groups). */
	if (sysfs_create_group(&client->dev.kobj, foo_groups[0]))
		dev_warn(&client->dev, "sysfs group creation failed\n");

	dev_info(&client->dev, "FOO chip ready at addr 0x%02x on %s\n",
		 client->addr, client->adapter->name);
	return 0;
}

static void foo_remove(struct i2c_client *client)
{
	/* Undo manual sysfs setup; devm_* state is freed automatically. */
	sysfs_remove_group(&client->dev.kobj, foo_groups[0]);
	dev_info(&client->dev, "FOO chip removed\n");
}

/* ================================================================== */
/* Matching tables: how the core binds this driver to a client.        */
/* Order of matching: OF (compatible) -> ACPI -> id_table.             */
/* ================================================================== */

static const struct of_device_id foo_of_match[] = {
	{ .compatible = "vendor,foo" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, foo_of_match);

static const struct i2c_device_id foo_id[] = {
	{ "foo", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, foo_id);

static struct i2c_driver foo_driver = {
	.driver = {
		.name           = "foo",
		.of_match_table = foo_of_match,
	},
	.probe    = foo_probe,
	.remove   = foo_remove,
	.id_table = foo_id,
};

/*
 * module_i2c_driver() expands to the module_init()/module_exit() pair that
 * call i2c_add_driver()/i2c_del_driver(). This is the standard boilerplate.
 */
module_i2c_driver(foo_driver);

MODULE_AUTHOR("Interview Prep");
MODULE_DESCRIPTION("Annotated example I2C client driver for a FOO chip");
MODULE_LICENSE("GPL");
