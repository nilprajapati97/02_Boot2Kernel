/*
 * Minimal efficient GPIO toggle kernel module
 * - Demonstrates requesting a GPIO, fast toggling loop, and safe cleanup.
 * - Shows optimization techniques for low-latency toggling (disable preemption,
 *   use raw descriptor APIs, avoid extra checks in hot path).
 *
 * Usage:
 *  insmod gpio_toggle.ko gpio=24 hz=1000 count=0
 *    gpio    - Linux global GPIO number (or leave unset and export via DT/plat)
 *    hz      - target toggle frequency in Hz (per-edge frequency)
 *    count   - number of full cycles to toggle (0 = forever)
 *
 * Notes:
 * - This is a generic consumer driver for demonstration. For the lowest latency
 *   on real SoCs, implement a gpio_chip that writes the controller registers
 *   directly (avoids gpiolib overhead) or use memory-mapped register access.
 * - gpiod_set_raw_value() is used to avoid any extra sleeps or locking in the
 *   hot path. The thread disables preemption when in the tight loop to reduce
 *   scheduling jitter; do this only when you know the critical section is short.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Assistant");
MODULE_DESCRIPTION("Minimal efficient GPIO toggle driver for interview/demo");
MODULE_VERSION("0.1");

static int gpio_num = -1; /* set as module parameter */
static unsigned int hz = 1000; /* toggles per second (edges; i.e., half-period) */
static unsigned int count = 0; /* number of cycles, 0 = infinite */

module_param(gpio_num, int, 0444);
MODULE_PARM_DESC(gpio_num, "Global GPIO number to toggle");
module_param(hz, uint, 0444);
MODULE_PARM_DESC(hz, "Toggle frequency in Hz (edges per second)");
module_param(count, uint, 0444);
MODULE_PARM_DESC(count, "Number of cycles to toggle (0 = forever)");

static struct task_struct *toggle_task;
static struct gpio_desc *gdesc;

/* Helper: sleep for microseconds with higher precision when needed */
static inline void short_busy_wait_us(unsigned int us)
{
    /* For sub-millisecond sleeps, using udelay is acceptable. For longer
     * periods, schedule() or msleep() should be used to avoid busy-waiting.
     */
    if (us < 20)
        udelay(us);
    else if (us < 2000)
        usleep_range(us, us + 10);
    else
        msleep((us + 500) / 1000);
}

static int gpio_toggle_thread(void *data)
{
    unsigned long half_period_us;
    unsigned int cycles = 0;
    bool val = 0;

    if (!gdesc) {
        pr_err("gpio_toggle: no gpio descriptor\n");
        return -EINVAL;
    }

    if (hz == 0) {
        pr_err("gpio_toggle: hz cannot be zero\n");
        return -EINVAL;
    }

    /* half period in microseconds (edge interval). e.g., for 1000 Hz -> 500 us */
    half_period_us = DIV_ROUND_CLOSEST(1000000UL, (unsigned long)hz);

    pr_info("gpio_toggle: starting toggler gpio=%d hz=%u half_period_us=%lu count=%u\n",
            gpio_num, hz, half_period_us, count);

    /* Main toggling loop */
    while (!kthread_should_stop()) {
        /* Optional stop when count specified */
        if (count && cycles >= count)
            break;

        /* Disable preemption to reduce jitter in tight toggle loop. Keep section short! */
        preempt_disable();

        /* HOT PATH: set value. Use raw API to reduce overhead (no sleepable checks).
         * gpiod_set_raw_value() performs the minimal work. On some platforms the
         * consumer may still incur controller register writes via gpiolib.
         */
        gpiod_set_raw_value(gdesc, val);

        /* busy-wait/sleep for half period */
        short_busy_wait_us((unsigned int)half_period_us);

        val = !val;

        /* restore preemption */
        preempt_enable();

        /* If frequency low, yield to scheduler to avoid hogging CPU */
        if (half_period_us > 2000)
            cond_resched();

        /* increment cycles when we complete a full period (toggle twice) */
        if (!val)
            cycles++;
    }

    pr_info("gpio_toggle: stopping toggler after %u cycles\n", cycles);
    return 0;
}

static int __init gpio_toggle_init(void)
{
    int ret;

    if (gpio_num < 0) {
        pr_err("gpio_toggle: gpio parameter not provided\n");
        return -EINVAL;
    }

    /* Convert global GPIO number -> descriptor (new gpiod API). */
    gdesc = gpio_to_desc(gpio_num);
    if (!gdesc) {
        pr_err("gpio_toggle: failed to get gpio descriptor for %d\n", gpio_num);
        return -ENODEV;
    }

    /* Request control of the line to avoid conflicts. Use active-high output.
     * We use gpio_request_one as a safe fallback for consumers that only have
     * a numeric gpio. On DT/ACPI-based platform devices, better to use
     * devm_gpiod_get() from a platform device probe instead.
     */
    ret = gpio_request_one(gpio_num, GPIOF_OUT_INIT_LOW, "gpio_toggle");
    if (ret) {
        pr_err("gpio_toggle: gpio_request_one failed %d\n", ret);
        return ret;
    }

    /* Now map descriptor again if needed (gpio_request_one doesn't return desc). */
    gdesc = gpio_to_desc(gpio_num);
    if (!gdesc) {
        pr_err("gpio_toggle: gpio_to_desc failed after request\n");
        gpio_free(gpio_num);
        return -ENODEV;
    }

    /* Start kernel thread to toggle */
    toggle_task = kthread_run(gpio_toggle_thread, NULL, "gpio_toggle/%d", gpio_num);
    if (IS_ERR(toggle_task)) {
        ret = PTR_ERR(toggle_task);
        pr_err("gpio_toggle: failed to create thread: %d\n", ret);
        gpio_free(gpio_num);
        return ret;
    }

    pr_info("gpio_toggle: module loaded\n");
    return 0;
}

static void __exit gpio_toggle_exit(void)
{
    if (toggle_task && !IS_ERR(toggle_task)) {
        kthread_stop(toggle_task);
        toggle_task = NULL;
    }

    if (gpio_num >= 0) {
        /* ensure line in a known state and free */
        gpio_set_value(gpio_num, 0);
        gpio_free(gpio_num);
    }

    pr_info("gpio_toggle: module unloaded\n");
}

module_init(gpio_toggle_init);
module_exit(gpio_toggle_exit);
