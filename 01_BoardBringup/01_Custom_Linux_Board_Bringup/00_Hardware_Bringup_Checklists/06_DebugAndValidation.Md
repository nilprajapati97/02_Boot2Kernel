🧪 6. Debugging and Validation
============================================================================
| Step                     | Description                                   |
| -------------------------|---------------------------------------------- |
| ✅ `dmesg` Check         | Look for driver probes, errors, IRQ failures. |
| ✅ `strace` or `ltrace`  | Debug failing user applications.              |
| ✅ GPIO/I2C Oscilloscope | Verify signaling integrity and timing.        |
| ✅ Lauterbach/OpenOCD    | Use for low-level tracing via JTAG.           |
| ✅ Kernel Panic Recovery | Test and handle kernel crash scenarios.       |


Here’s a **deep dive explanation** of **Debugging and Validation (Step 6)** for a **Custom Embedded Linux Board**, especially suited for someone with 10 years of embedded Linux/Android experience, focusing on board bring-up and BSP development.

---

## 🧪 6. Debugging and Validation – Deep Dive

When you bring up a **custom embedded Linux board**, debugging and validation is **critical** to ensure all hardware-software interactions work reliably. Here’s a breakdown of each point with **real-world usage**.

---

### ✅ `dmesg` Check – Kernel Log Analyzer
====================================================================================================================================
**Purpose:** Identify kernel messages, driver probes, and errors during boot/runtime.

#### What You Look For:

* Whether your custom drivers are **loading and probing** correctly.
* Peripheral init failures like:
  `i2c i2c-2: adapter timeout`, `irq 49: nobody cared!`, `mmcblk0: error -110`.

#### Commands:

```bash
dmesg | grep -i "camera"
dmesg | grep -i "i2c" | less
```

#### Example:

You're integrating a camera over I2C and MIPI-CSI. A failing I2C read from `0x36` would show:

```bash
[  3.245671] i2c i2c-1: read failed at addr 0x36
```

You now know the I2C bus or camera power regulator needs fixing.

---

### ✅ `strace` / `ltrace` – User-Space Debugging Tools
====================================================================================================================================
**Purpose:** Trace system/library calls made by user-space applications.

* `strace` → Traces **syscalls** (e.g., `open()`, `read()`, `ioctl()`)
* `ltrace` → Traces **library calls** (e.g., `printf()`, `malloc()`)

#### When To Use:

* Application crashing or hanging
* Debugging HAL daemons like `cameraserver`, `audiohalservice`, etc.

#### Example:

```bash
strace ./my_camera_test_app
```

Output:

```bash
open("/dev/video0", O_RDWR) = -1 ENOENT (No such file or directory)
```

➤ Now you know your driver didn’t register `video0`.

---

### ✅ GPIO/I2C Oscilloscope – Hardware-Level Signal Validation
====================================================================================================================================
**Purpose:** Physically validate digital signaling between SoC and peripherals.

#### Tools Used:

* Oscilloscope or Logic Analyzer (e.g., Saleae Logic, Tektronix, Rigol)

#### Use Cases:

* I2C not responding despite software configuration.
* Verify **correct timing and voltage levels**.
* Check **interrupt activity** on GPIOs.

#### Example:

You're initializing a touchscreen controller on `I2C1`, but there's no ACK on scope:

* Possible root cause: missing regulator enable GPIO or pinmux not set in device tree.

---

### ✅ Lauterbach / OpenOCD – JTAG Debugging
====================================================================================================================================
**Purpose:** Low-level debug of bootloader, kernel, and hardware bring-up using **JTAG**.

#### Used For:

* Debugging **boot hangs** before UART is up.
* Stepping through U-Boot or SPL (MLO) code.
* Memory-mapped I/O register access.
* Setting breakpoints, examining backtrace, dumping RAM.

#### Lauterbach Example:

```c
d.load.elf u-boot.elf
d.break.set main
d.run
```

#### OpenOCD Example:

```bash
openocd -f board/ti_am335x.cfg
gdb-multiarch u-boot
(gdb) target remote :3333
(gdb) break board_init_r
```

---

### ✅ Kernel Panic Recovery – Test and Handle Kernel Crashes
====================================================================================================================================
**Purpose:** Detect, log, and recover from kernel panics or oopses.

#### Approach:

* Use `panic=5` bootarg to **auto-reboot**.
* Enable **kexec/kdump** to log crashes.
* Use **RAM console** or **persistent storage** to log panics (like Qualcomm’s ramoops).

#### Example:

```bash
dmesg | grep -i oops
```

You see:

```bash
Unable to handle kernel NULL pointer dereference at virtual address 00000000
```

Root cause: invalid pointer in a kernel driver during `probe()`.

---

## 🔁 Debugging Workflow Summary
====================================================================================================================================
| Phase               | Tools/Steps Used                                   |
| ------------------- | -------------------------------------------------- |
| Bootloader issues   | JTAG (Lauterbach/OpenOCD), UART, `u-boot` prints   |
| Kernel driver debug | `dmesg`, GPIO scope, `printk()` instrumentation    |
| User-space issues   | `strace`, `ltrace`, `logcat` (Android)             |
| Peripheral failure  | I2C/SPI protocol analysis with oscilloscope        |
| Crash analysis      | `kdump`, `ramoops`, `panic=`, stack trace via JTAG |

---

## 🧠 Interview Note – What to Say
====================================================================================================================================
> “In one of my custom embedded Linux board projects based on the Qualcomm SDM660, I used a mix of `dmesg`, `strace`, and hardware-level probing with oscilloscopes to bring up and validate camera, audio, and touchscreen interfaces. For early boot failures, I used OpenOCD with GDB to trace SPL and U-Boot execution. To handle kernel crashes, I integrated ramoops and persistent logs to collect panic information after reboots, which helped in fixing a memory corruption issue in our camera driver.”

