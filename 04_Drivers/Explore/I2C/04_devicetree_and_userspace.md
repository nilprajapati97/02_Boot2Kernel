# Device Tree Bindings & User-Space Recipes

This file shows how a chip gets described to the kernel and how to poke the bus
from user space — the practical glue around the two C drivers in this folder.

---

## 1. Device Tree: describing the controller and its chips

On embedded/ARM systems the device tree (DT) is the primary way the kernel learns
what hardware exists. The I2C **controller** is a node; each **chip** on that bus
is a child node whose `reg` is the 7-bit slave address.

```dts
/* SoC .dtsi usually defines the controller; the board .dts enables it
 * and adds the chips that are actually soldered on this board.          */

&i2c1 {                                   /* the controller (adapter)    */
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_i2c1>;          /* SDA/SCL pin muxing          */
    clock-frequency = <400000>;           /* run the bus at 400 kHz      */
    status = "okay";                      /* enable this controller      */

    /* --- chip #1: our FOO example device at address 0x48 --- */
    foo_sensor: sensor@48 {
        compatible = "vendor,foo";        /* matches foo_of_match[]      */
        reg = <0x48>;                     /* 7-bit I2C address           */
        interrupt-parent = <&gpio2>;      /* optional IRQ line           */
        interrupts = <5 IRQ_TYPE_LEVEL_LOW>;
        vdd-supply = <&reg_3v3>;          /* optional regulator          */
    };

    /* --- chip #2: an EEPROM at address 0x50 --- */
    eeprom@50 {
        compatible = "atmel,24c256";
        reg = <0x50>;
        pagesize = <64>;
    };
};
```

Key points to say in an interview:
- `reg = <0xADDR>` **is** the 7-bit slave address, and it must match the
  `@ADDR` in the node name (the "unit address").
- `compatible` is the primary match key against the driver's `of_match_table`.
- When the adapter driver calls `i2c_add_adapter()`, the **core walks these child
  nodes** and creates an `i2c_client` for each — your client driver's `.probe()`
  then fires for the ones it matches.
- `clock-frequency` on the **controller** node sets the SCL speed.
- IRQs, regulators, GPIOs, pinctrl are all wired here, not in the driver.

### Controller node (defined by the SoC, shown for completeness)

```dts
i2c1: i2c@21a0000 {
    compatible = "vendor,foo-i2c";        /* matches foo_i2c_of_match[]  */
    reg = <0x021a0000 0x4000>;            /* MMIO register window        */
    interrupts = <GIC_SPI 36 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&clks IMX6QDL_CLK_I2C1>;
    #address-cells = <1>;                 /* children use 1-cell reg     */
    #size-cells = <0>;                    /* (addresses, no size)        */
    status = "disabled";                  /* board .dts flips to "okay"  */
};
```

---

## 2. Verifying it bound correctly

```bash
# Is my client driver bound to a device?
ls /sys/bus/i2c/drivers/foo/
#   -> shows e.g. "1-0048" (bus 1, address 0x48) if probe succeeded

# Which driver owns a given device?
readlink /sys/bus/i2c/devices/1-0048/driver
#   -> .../drivers/foo

# Kernel messages from probe
dmesg | grep -i foo
```

The directory name format `B-XXXX` = **bus number B**, **address 0xXXXX**.

---

## 3. User-space access with i2c-tools

Install: `apt install i2c-tools` (Debian/Ubuntu) — needs the `i2c-dev` module.

```bash
modprobe i2c-dev            # creates /dev/i2c-* char devices

i2cdetect -l                # list all I2C buses in the system
i2cdetect -y 1              # scan bus 1; show addresses that ACK

# read register 0x0F (WHOAMI in our FOO example) of device 0x48 on bus 1
i2cget -y 1 0x48 0x0F

# write 0x01 to register 0x01 of device 0x48 on bus 1
i2cset -y 1 0x48 0x01 0x01

# read a word (16-bit) from register 0x00
i2cget -y 1 0x48 0x00 w
```

> If a kernel driver already claims address 0x48, `i2cget` may show `UU`
> (in-use). That is expected and good — it means probe succeeded.

---

## 4. Instantiating / removing a device from user space

Useful during bring-up when you don't yet have a DT entry:

```bash
# create an instance of driver "foo" at address 0x48 on bus 1
echo foo 0x48 > /sys/bus/i2c/devices/i2c-1/new_device

# remove it
echo 0x48      > /sys/bus/i2c/devices/i2c-1/delete_device
```

---

## 5. Talking to a chip directly via /dev/i2c-N (C)

```c
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    /* select slave 0x48 (use I2C_SLAVE_FORCE to override a bound driver) */
    if (ioctl(fd, I2C_SLAVE, 0x48) < 0) { perror("I2C_SLAVE"); return 1; }

    /* register-pointer write, then read: the classic 2-step */
    uint8_t reg = 0x00;
    if (write(fd, &reg, 1) != 1) { perror("write reg"); return 1; }

    uint8_t data[2];
    if (read(fd, data, 2) != 2)  { perror("read");     return 1; }

    printf("value = %u\n", (data[0] << 8) | data[1]);
    close(fd);
    return 0;
}
```

For SMBus-style single calls there is also the `i2c_smbus_*` user-space API in
`<linux/i2c-dev.h>` / `i2c/smbus.h` (e.g. `i2c_smbus_read_byte_data(fd, reg)`).

---

## 6. Building the kernel modules in this folder

```makefile
# Makefile
obj-m += 02_i2c_client_driver.o
obj-m += 03_i2c_adapter_driver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
```

```bash
make
sudo insmod 02_i2c_client_driver.ko
dmesg | tail
sudo rmmod 02_i2c_client_driver
```

> The adapter driver (`03_*`) is a *skeleton with pseudo-hardware register
> access* — it will compile but is meant for study, not for loading on real
> hardware unless you replace the register map with a real controller's.
