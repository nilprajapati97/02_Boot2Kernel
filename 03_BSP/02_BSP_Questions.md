Here are **commonly asked and in-depth interview questions** (with brief answers) for a **10-year experienced Linux BSP Developer** role:

---

## 🔧 **BSP and Bootloader**

### 1. **What is a BSP in embedded Linux?**

**Answer:** A BSP (Board Support Package) is a collection of software and configuration files tailored to a specific hardware board. It typically includes:

* Bootloader (U-Boot/SPL)
* Device tree
* Kernel configuration
* Drivers (e.g., GPIO, I2C, SPI, UART)
* Root filesystem

---

### 2. **Explain U-Boot boot flow.**

**Answer:**

1. **SPL**: Initializes DRAM, minimal hardware.
2. **U-Boot proper**: Loads kernel image, device tree, and rootfs.
3. **Executes** `bootm`/`bootz` to start kernel.

---

### 3. **What’s the difference between SPL and U-Boot proper?**

**Answer:**

* **SPL (Secondary Program Loader)**: Minimal loader to init RAM, load full U-Boot.
* **U-Boot proper**: Full bootloader, provides environment shell, loads kernel/DTB/rootfs.

---

### 4. **How do you port U-Boot to a new board?**

**Answer:**

* Create board directory under `board/<vendor>/<board>/`
* Add defconfig in `configs/`
* Configure DDR, clocks, peripherals in SPL
* Write board-specific initialization in `board.c`
* Update device tree for U-Boot

---

## 📦 **Device Tree**

### 5. **What is the role of device tree in Linux BSP?**

**Answer:**
The device tree describes the hardware to the kernel (CPU, memory, buses, devices) without hardcoding it. Used during kernel boot.

---

### 6. **How do you debug a missing device in Linux from device tree?**

**Answer:**

* Check `dmesg` logs for device driver probe errors.
* Verify `compatible`, `reg`, `interrupts` in DT.
* Ensure the driver is compiled and enabled in `.config`.

---

## 🧰 **Kernel Configuration and Drivers**

### 7. **What changes are needed in the kernel to enable a new peripheral (say I2C sensor)?**

**Answer:**

* Add DT node for sensor under correct I2C controller
* Enable required driver in `menuconfig`
* Confirm correct bus/address
* Possibly add platform data or module parameters

---

### 8. **How do you debug kernel boot crash on a custom board?**

**Answer:**

* Check UART logs, panic messages
* Use `earlyprintk` or `initcall_debug`
* Use JTAG or Lauterbach debugger
* Validate DDR init and clock settings

---

## 💽 **RootFS and Init System**

### 9. **Which rootfs do you use? How do you build it?**

**Answer:**

* Common: BusyBox, Buildroot, Yocto
* Build using `buildroot` or Yocto `bitbake`, customize `image recipe`
* Add custom init scripts and kernel modules

---

## 🧪 **Testing and Debugging**

### 10. **How do you test BSP stability and performance?**

**Answer:**

* Boot stress tests (power cycling)
* Peripheral I/O tests (dd, i2c-tools, etc.)
* Thermal/stress test using `stress-ng`
* Use `ftrace`, `perf`, `kgdb` for profiling/debugging

---

### 11. **What is earlycon and earlyprintk?**

**Answer:**

* Used to get early kernel boot logs before console driver is loaded.
* Passed as kernel command line: `earlyprintk=serial,ttyS0,115200` or `earlycon`

---

## 📜 **Misc Advanced Questions**

### 12. **Explain how a new DDR is integrated and initialized in U-Boot.**

**Answer:**

* Update DDR timing and PHY config in SPL
* Use vendor-provided training code if needed
* Validate with memory test (`mtest`)

---

### 13. **How do you secure the boot process in Linux?**

**Answer:**

* Use secure boot with signed U-Boot and kernel
* Use hardware crypto engines (e.g., TrustZone)
* Enable U-Boot verified boot

---

### 14. **How do you reduce kernel size?**

**Answer:**

* Disable unused modules via `menuconfig`
* Use `CONFIG_EXPERT` and `CONFIG_EMBEDDED`
* Remove debug symbols

---

Would you like a **PDF cheat sheet**, **Yocto-specific questions**, or **board bring-up checklist**?
