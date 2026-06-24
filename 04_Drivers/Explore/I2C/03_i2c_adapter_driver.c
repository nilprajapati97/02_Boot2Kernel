// SPDX-License-Identifier: GPL-2.0
/*
 * 03_i2c_adapter_driver.c
 *
 * A heavily-annotated I2C ADAPTER (bus / controller) driver SKELETON for a
 * fictional SoC I2C controller "FOO-I2C". This is the OTHER half of the
 * subsystem: it knows the controller's registers and turns an array of
 * struct i2c_msg into real START / address / data / STOP signaling.
 *
 * It is a PLATFORM driver, probed from the device tree node that describes
 * the controller. When it registers its i2c_adapter, the I2C core
 * automatically walks the controller's child DT nodes and instantiates a
 * client (i2c_client) for every chip described there.
 *
 * NOTE: register access here is illustrative pseudo-hardware. Replace the
 * FOO_REG_* writes/reads and the wait loops with your real controller's
 * programming model (IRQ-driven or DMA in production).
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/of.h>

/* ------------------------------------------------------------------ */
/* Imaginary controller register map                                   */
/* ------------------------------------------------------------------ */
#define FOO_CR      0x00   /* control: enable, START, STOP, R/W        */
#define FOO_SR      0x04   /* status:  busy, ACK, done, arb-lost       */
#define FOO_DR      0x08   /* data register (TX/RX FIFO of depth 1)    */
#define FOO_ADDR    0x0C   /* slave address shadow                     */
#define FOO_DIV     0x10   /* clock divider for SCL frequency          */

#define FOO_CR_EN     BIT(0)
#define FOO_CR_START  BIT(1)
#define FOO_CR_STOP   BIT(2)
#define FOO_CR_RD     BIT(3)   /* 1 = read, 0 = write                  */
#define FOO_CR_TX     BIT(4)   /* kick a byte out of FOO_DR            */

#define FOO_SR_BUSY   BIT(0)
#define FOO_SR_DONE   BIT(1)   /* byte transfer complete               */
#define FOO_SR_ACK    BIT(2)   /* 1 = ACK received from slave          */
#define FOO_SR_ARB    BIT(3)   /* arbitration lost                     */

/* ------------------------------------------------------------------ */
/* Per-controller state                                                */
/* ------------------------------------------------------------------ */
struct foo_i2c {
	struct i2c_adapter adap;
	struct device     *dev;
	void __iomem      *base;   /* mapped MMIO registers               */
	struct clk        *clk;
	u32                bus_hz; /* requested SCL frequency             */
};

static inline void foo_wr(struct foo_i2c *i2c, u32 reg, u32 val)
{
	writel(val, i2c->base + reg);
}
static inline u32 foo_rd(struct foo_i2c *i2c, u32 reg)
{
	return readl(i2c->base + reg);
}

/* Wait until the controller signals "byte done"; return 0 or -ETIMEDOUT. */
static int foo_wait_done(struct foo_i2c *i2c)
{
	u32 sr;
	/* poll SR.DONE with a 50ms timeout (use IRQ completion in real HW) */
	return readl_poll_timeout(i2c->base + FOO_SR, sr,
				  (sr & FOO_SR_DONE), 1, 50 * 1000);
}

/* ================================================================== */
/* The heart of the driver: master_xfer                                */
/* Process an array of i2c_msg. Issue (repeated) START before each msg,*/
/* exactly one STOP at the end. Return num on success, -errno on fail.  */
/* ================================================================== */

static int foo_xfer_one(struct foo_i2c *i2c, struct i2c_msg *m, bool repeated)
{
	u32 cr;
	int i, ret;

	/* program 7-bit address + R/W bit, then assert (repeated) START */
	foo_wr(i2c, FOO_ADDR, (m->addr << 1) | !!(m->flags & I2C_M_RD));
	cr = FOO_CR_EN | FOO_CR_START;
	if (m->flags & I2C_M_RD)
		cr |= FOO_CR_RD;
	foo_wr(i2c, FOO_CR, cr);

	ret = foo_wait_done(i2c);
	if (ret)
		return ret;

	/* address phase must be ACKed (unless this is a 0-length probe) */
	if (!(foo_rd(i2c, FOO_SR) & FOO_SR_ACK) && m->len)
		return -ENXIO;          /* no device at this address       */

	for (i = 0; i < m->len; i++) {
		if (m->flags & I2C_M_RD) {
			/* trigger a read of one byte, then pull it from DR */
			foo_wr(i2c, FOO_CR, FOO_CR_EN | FOO_CR_RD | FOO_CR_TX);
			ret = foo_wait_done(i2c);
			if (ret)
				return ret;
			m->buf[i] = foo_rd(i2c, FOO_DR) & 0xff;
		} else {
			/* load DR and push the byte out */
			foo_wr(i2c, FOO_DR, m->buf[i]);
			foo_wr(i2c, FOO_CR, FOO_CR_EN | FOO_CR_TX);
			ret = foo_wait_done(i2c);
			if (ret)
				return ret;
			if (!(foo_rd(i2c, FOO_SR) & FOO_SR_ACK))
				return -EIO;   /* slave NACKed a data byte */
		}
	}
	return 0;
}

static int foo_master_xfer(struct i2c_adapter *adap,
			   struct i2c_msg *msgs, int num)
{
	struct foo_i2c *i2c = i2c_get_adapdata(adap);
	int i, ret;

	for (i = 0; i < num; i++) {
		ret = foo_xfer_one(i2c, &msgs[i], /*repeated=*/ i != 0);
		if (ret) {
			foo_wr(i2c, FOO_CR, FOO_CR_EN | FOO_CR_STOP);
			return ret;     /* abort: emit STOP, report errno   */
		}
	}

	foo_wr(i2c, FOO_CR, FOO_CR_EN | FOO_CR_STOP);  /* single STOP      */

	/*
	 * Contract: return the number of messages successfully transferred.
	 * The core checks this equals 'num'.
	 */
	return num;
}

/*
 * Advertise capabilities. We support plain I2C and (via the core's
 * emulation, but we still advertise here) the common SMBus byte/word ops.
 */
static u32 foo_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL);
}

static const struct i2c_algorithm foo_algo = {
	.master_xfer   = foo_master_xfer,
	.functionality = foo_functionality,
};

/* ================================================================== */
/* platform probe / remove                                             */
/* ================================================================== */

static int foo_i2c_probe(struct platform_device *pdev)
{
	struct foo_i2c *i2c;
	int ret;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->dev = &pdev->dev;

	/* 1. Map the controller's MMIO register window. */
	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	/* 2. Enable the controller's functional clock. */
	i2c->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(i2c->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(i2c->clk),
				     "failed to get clock\n");

	/* 3. Read the requested bus speed from DT (default 100 kHz). */
	if (of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				 &i2c->bus_hz))
		i2c->bus_hz = 100000;

	/* 4. Program clock divider / enable the controller (HW-specific). */
	foo_wr(i2c, FOO_DIV, clk_get_rate(i2c->clk) / (i2c->bus_hz * 4));
	foo_wr(i2c, FOO_CR, FOO_CR_EN);

	/* 5. Fill in and register the i2c_adapter. */
	i2c_set_adapdata(&i2c->adap, i2c);
	i2c->adap.owner   = THIS_MODULE;
	i2c->adap.algo    = &foo_algo;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = pdev->dev.of_node;  /* lets core scan kids */
	i2c->adap.nr      = -1;                       /* dynamic bus number */
	i2c->adap.timeout = HZ / 10;
	i2c->adap.retries = 3;
	strscpy(i2c->adap.name, "FOO I2C adapter", sizeof(i2c->adap.name));

	platform_set_drvdata(pdev, i2c);

	/*
	 * i2c_add_adapter() registers the bus AND triggers the core to
	 * enumerate child DT nodes -> creating i2c_clients for each chip.
	 * Use devm_i2c_add_adapter() so it is removed automatically.
	 */
	ret = devm_i2c_add_adapter(&pdev->dev, &i2c->adap);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to add I2C adapter\n");

	dev_info(&pdev->dev, "FOO I2C bus %d @ %u Hz registered\n",
		 i2c->adap.nr, i2c->bus_hz);
	return 0;
}

/* With devm_i2c_add_adapter + devm_* resources, no explicit remove needed. */

static const struct of_device_id foo_i2c_of_match[] = {
	{ .compatible = "vendor,foo-i2c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, foo_i2c_of_match);

static struct platform_driver foo_i2c_driver = {
	.probe  = foo_i2c_probe,
	.driver = {
		.name           = "foo-i2c",
		.of_match_table = foo_i2c_of_match,
	},
};
module_platform_driver(foo_i2c_driver);

MODULE_AUTHOR("Interview Prep");
MODULE_DESCRIPTION("Annotated example I2C adapter (controller) driver");
MODULE_LICENSE("GPL");
