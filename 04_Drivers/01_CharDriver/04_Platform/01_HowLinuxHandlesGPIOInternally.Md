✏️ How Linux Handles GPIO Internally — A Must-Know for BSP Engineer...
If you’ve ever used gpio_set_value() in Embedded Linux and wondered how the kernel knows exactly which pin to toggle.

Let’s walk through how the Linux GPIO core maps a logical GPIO number to a real hardware pin, using the internal structure of the GPIO Subsystem.

🧱 Step 1: Registering the GPIO Controller in Your Driver
===============================================================================================

Your SoC (e.g., TI AM335x, STM32, i.MX) exposes GPIOs via a controller. You represent it using struct gpio_chip in your platform driver:

struct gpio_chip chip = {
 .label = "gpio-ti",
 .ngpio = 32,
 .base = -1, // Request dynamic base
 .direction_output = ti_gpio_dir_out,
 .get = ti_gpio_get,
 .set = ti_gpio_set,
};

In .probe():
            struct ti_priv {
                                void __iomem *base;
                                struct gpio_chip chip;
            };

priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
priv->base = devm_ioremap_resource(&pdev->dev, res);
chip.parent = &pdev->dev;
gpiochip_add_data(&chip, priv); // Registers with GPIO core

🔍 What gpiochip_add_data() function do Internally ?
===============================================================================================
This core function (from gpiolib.c) integrates your controller into the Linux GPIO subsystem:

✅ Internally it:
Dynamically assigns a GPIO base number (e.g., 32)
Fills the global gpio_desc[] array: gpio_desc[base + i].chip = &chip;
Stores your driver’s private data (priv) so that callbacks can access registers
Adds your chip to the global GPIO list

Exposes it via /sys/class/gpio and debugfs (if enabled)
🌀 Step 2: Toggling a GPIO from Application or Driver
Suppose user-space or a driver calls:
                                gpio_set_value(53, 1);
                                Here’s what happens internally:
                                desc = gpio_desc[53];
                                chip = desc->chip;
                                offset = 53 - chip->base;
                                chip->set(chip, offset, 1); // Calls your driver’s set function

⚙️ Your Controller Driver Implements .set()

void ti_gpio_set(struct gpio_chip *chip, unsigned offset, int val)
{
         struct ti_priv *priv = gpiochip_get_data(chip);
         u32 reg = readl(priv->base + GPIO_DATAOUT);

         if (val)
         reg |= BIT(offset);
         else
         reg &= ~BIT(offset);

         writel(reg, priv->base + GPIO_DATAOUT);
}
✅ And that’s how the physical pin changes on your hardware. Simple for the caller, powerful behind the scenes.

🧠 Behind-the-Scenes Flow
===============================================================================================

gpio_set_value(53, 1)
 ↓
gpio_desc[53] → chip = &ti_gpio_chip
 ↓
offset = 53 - chip->base = 21
 ↓
chip->set(chip, 21, 1) → ti_gpio_set()
 ↓
writel() to hardware register
 ↓
⚡ Real GPIO pin toggled

🔑 Key Takeaways...
===============================================================================================
gpiochip_add_data(): Registers your GPIO controller with the Linux kernelgpio_desc[]: Maps global GPIO numbers to your controller
chip.set(), .get() : Your hardware-specific callbacks
gpiochip_get_data(): Fetches private driver context (struct ti_priv)
gpio_set_value(n, v): Generic API routed to the right chip + offset
