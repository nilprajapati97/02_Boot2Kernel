🔌 5. Peripheral Bring-Up
=====================================================================================
| Peripheral            | Checklist                                                  |
| ----------------------|----------------------------------------------------------- |
| ✅ UART               | Console works? Flow control set correctly?                 |
| ✅ I2C                | Probe with `i2cdetect`, enable EEPROM/sensors.             |
| ✅ SPI                | Loopback or flash device read/write.                       |
| ✅ GPIO               | Export via sysfs or `libgpiod`, test toggling.             |
| ✅ MMC/eMMC           | Read/write partitions, detect via `lsblk`.                 |
| ✅ USB                | Detect device insertion (`dmesg`), test storage or serial. |
| ✅ Ethernet           | DHCP/static config, ping test, iperf3.                     |
| ✅ Display (optional) | Enable framebuffer or DRM, check splash screen or GUI.     |

Here's an **in-depth breakdown** of the **🔌 Peripheral Bring-Up** process on an **Embedded Linux Custom Board** — a critical phase after rootfs is operational, ensuring all hardware blocks work as expected.

---

## 🔌 5. Peripheral Bring-Up – In Depth

Peripheral bring-up means validating and testing the hardware interfaces (UART, I2C, SPI, etc.) integrated into the board using drivers, user-space tools, and logs.

---

### ✅ **UART (Serial Console)**
=================================================================================================================
#### 🔹 Purpose:

* Main debug interface during bring-up.
* Can be used for bootloader logs, kernel logs, and login shell.

#### 🔹 Checklist:

* Bootloader and kernel should have:

  ```
  console=ttyS0,115200
  ```
* UART flow control (RTS/CTS) should be disabled/enabled as per board design.

#### 🔹 Test:

* Connect to host via minicom, screen, or picocom:

  ```bash
  screen /dev/ttyUSB0 115200
  ```
* Expect:

  * Boot logs.
  * Login prompt.
  * Echo of characters.

---

### ✅ **I2C (Inter-Integrated Circuit)**
=================================================================================================================

#### 🔹 Purpose:

Used to communicate with sensors, EEPROMs, RTCs, etc.

#### 🔹 Device Tree Sample:

```dts
&i2c1 {
    status = "okay";
    eeprom@50 {
        compatible = "atmel,24c32";
        reg = <0x50>;
    };
};
```

#### 🔹 Test:

* Load driver:

  ```bash
  modprobe i2c-dev
  ```
* List buses:

  ```bash
  ls /dev/i2c-*
  ```
* Detect devices:

  ```bash
  i2cdetect -y 0
  ```
* Read EEPROM:

  ```bash
  i2cdump -y 0 0x50
  ```

---

### ✅ **SPI (Serial Peripheral Interface)**
=================================================================================================================
#### 🔹 Purpose:

Used for high-speed comm with flash memory, displays, ADCs, etc.

#### 🔹 Device Tree Sample:

```dts
&spi0 {
    status = "okay";
    flash@0 {
        compatible = "spi-flash";
        reg = <0>;
        spi-max-frequency = <25000000>;
    };
};
```

#### 🔹 Loopback Test:

* Connect MISO to MOSI.
* Use `spidev` driver and test:

  ```bash
  echo "test" > /dev/spidev0.0
  ```

#### 🔹 Flash Test:

* Use `mtd-utils` to read/write:

  ```bash
  flash_erase /dev/mtd0 0 0
  nandwrite /dev/mtd0 test.bin
  ```

---

### ✅ **GPIO (General Purpose I/O)**
=================================================================================================================
#### 🔹 Options:

* Sysfs (deprecated in newer kernels)
* `libgpiod` (preferred)

#### 🔹 Test (Sysfs-based):

```bash
echo 24 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio24/direction
echo 1 > /sys/class/gpio/gpio24/value
```

#### 🔹 libgpiod Test:

```bash
gpiodetect
gpioset gpiochip0 24=1
```

#### 🔹 Validate:

* Use multimeter or scope to verify toggling.
* Observe interrupt trigger on input GPIOs.

---

### ✅ **MMC / eMMC**
=================================================================================================================
#### 🔹 Purpose:

Storage devices on SD or embedded storage.

#### 🔹 Checklist:

* Device Tree:

```dts
&mmc1 {
    status = "okay";
    bus-width = <4>;
    cd-gpios = <&gpio1 2 GPIO_ACTIVE_LOW>;
};
```

#### 🔹 Test:

* On boot, verify detection via:

  ```bash
  dmesg | grep mmc
  lsblk
  ```
* Mount and test:

  ```bash
  mount /dev/mmcblk0p1 /mnt
  touch /mnt/testfile
  ```

---

### ✅ **USB**
=================================================================================================================
#### 🔹 Purpose:

For USB devices (flash drives, Wi-Fi, serial gadgets, etc.)

#### 🔹 Checklist:

* Kernel config: `CONFIG_USB_*`, `CONFIG_USB_STORAGE`, etc.
* Device Tree:

```dts
&usb0 {
    status = "okay";
};
```

#### 🔹 Test:

* Plug a USB drive and check:

  ```bash
  dmesg | tail
  ls /dev/sd*
  mount /dev/sda1 /mnt
  ```
* For USB serial (e.g. FTDI):

  ```bash
  dmesg | grep ttyUSB
  ```

---

### ✅ **Ethernet**
=================================================================================================================
#### 🔹 Checklist:

* Ensure PHY is properly configured in DT:

```dts
&eth0 {
    phy-handle = <&phy0>;
    status = "okay";
};
```

#### 🔹 Bring-up:

* Dynamic IP:

  ```bash
  udhcpc -i eth0
  ```
* Static:

  ```bash
  ifconfig eth0 192.168.1.100
  route add default gw 192.168.1.1
  ```

#### 🔹 Test:

```bash
ping 8.8.8.8
iperf3 -s (on host)
iperf3 -c <host_ip>
```

---

### ✅ **Display (Optional)**
=================================================================================================================
#### 🔹 Display Type:

* HDMI, LVDS, RGB, MIPI-DSI.

#### 🔹 Modes:

* Framebuffer (`/dev/fb0`)
* DRM/KMS (`/dev/dri/card0`)
* Wayland/Weston/X11 GUI

#### 🔹 Device Tree:

```dts
&lcdif {
    status = "okay";
    display = <&panel>;
};
```

#### 🔹 Test:

* For framebuffer:

  ```bash
  cat /dev/urandom > /dev/fb0
  ```
* For GUI (if Wayland):

  ```bash
  weston-launch
  ```

---

## 📊 Summary Table
=================================================================================================================
| Peripheral | Tools/Commands                 | What to Check                      |
| ---------- | ------------------------------ | ---------------------------------- |
| UART       | `screen`, `console=...`        | Boot logs, shell                   |
| I2C        | `i2cdetect`, `i2cdump`         | Devices respond at address         |
| SPI        | `spidev`, `flash_erase`, etc.  | Loopback, SPI flash read/write     |
| GPIO       | `libgpiod`, `sysfs`            | Can toggle, read input, detect IRQ |
| MMC/eMMC   | `lsblk`, `mount`               | Read/write works                   |
| USB        | `dmesg`, `mount`, `lsusb`      | Devices detected                   |
| Ethernet   | `ping`, `ifconfig`, `iperf3`   | Connectivity works                 |
| Display    | `fbtest`, `weston`, `/dev/fb0` | See splash screen or UI            |

---

