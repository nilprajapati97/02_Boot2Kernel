✅ 2. U-Boot Porting
==============================================================================================================================
🔹 Goal:
        Full-fledged bootloader that initializes all peripherals and loads Linux kernel + DTB.

Key Activities:
----------------------------------------------------------------------------------------------------
| Task             | Description                                                                   |
| ---------------- | ----------------------------------------------------------------------------- |
| MMC/eMMC Support | Enable SDHC/SDMMC driver for your SoC. (`CONFIG_MMC`, `CONFIG_MMC_SDHCI_...`) |
| USB Controller   | Enable USB Host + Gadget (`CONFIG_USB_GADGET`, `CONFIG_USB_DWC3`)             |
| SPI/NAND Flash   | Use if you're booting from SPI-NOR or NAND. Port driver and memory mapping.   |
| Ethernet         | For TFTP/NFS, enable `CONFIG_NET`, `CONFIG_PHYLIB`, `CONFIG_DM_ETH`, etc.     |
| PMIC/Regulator   | If needed, interface with I2C/SPI PMIC for voltages (via PMIC driver)         |
| Device Tree      | Create proper U-Boot DTB or reuse Linux DTB (trim unnecessary nodes)          |
| Board.c          | Setup pinmux, power rails, any GPIOs needed before kernel                     |



Here’s an **in-depth breakdown** of each step in 
✅ **U-Boot Porting (Stage-2 Bootloader)**, based on real embedded Linux BSP workflows, particularly for boards like **Qualcomm SDM660** or similar ARM-based platforms. U-Boot (second-stage bootloader) is responsible for initializing full hardware and loading the Linux kernel with DTB and optional initrd.
==============================================================================================================================

## 🔹 Overview of U-Boot Responsibilities
==============================================================================================================================

After the minimal SPL (MLO) has done:

* DRAM Init
* Stack & UART setup
* Loading U-Boot to DRAM

Now **U-Boot proper** runs:

* Initializes peripherals
* Parses bootargs
* Loads kernel + DTB from storage (MMC/USB/NAND/TFTP)
* Hands off to Linux via `bootm` or `booti`

---

## ✅ Task-by-Task Deep Explanation
==============================================================================================================================

### 1. **MMC/eMMC Support**

🔹 **Why?**
To read Linux kernel, DTB, and rootfs/initrd from SD/eMMC storage.

🔹 **Key Kconfig:**

```c
CONFIG_MMC=y
CONFIG_MMC_SDHCI=y
CONFIG_MMC_SDHCI_MSM=y          // For Qualcomm SDHCI
CONFIG_MMC_MUX=y
CONFIG_GENERIC_MMC=y
CONFIG_DM_MMC=y
```

🔹 **Device Tree Snippet:**

```dts
&sdhc_1 {
    status = "okay";
    bus-width = <8>;
    mmc-hs400-1_8v;
    vmmc-supply = <&vreg_sd>;
};
```

🔹 **Driver Path in U-Boot:**
`drivers/mmc/mmc.c`, `drivers/mmc/sdhci.c`, `drivers/mmc/sdhci-msm.c`

🔹 **Log Sample:**

```
mmc0: SDHCI controller on sdhci@7824900 [mmc0] using ADMA
MMC:   mmc@7824900: 1 (SD)
```

---

### 2. **USB Controller (Host + Gadget)**
==============================================================================================================================

🔹 **Why?**
To support USB boot, USB-based flashing (fastboot), or host mode for storage.

🔹 **Kconfig:**

```c
CONFIG_USB=y
CONFIG_DM_USB=y
CONFIG_USB_DWC3=y
CONFIG_USB_DWC3_GADGET=y
CONFIG_USB_GADGET=y
CONFIG_CMD_USB=y
CONFIG_CMD_USB_MASS_STORAGE=y
```

🔹 **Device Tree:**

```dts
usb@6a00000 {
    compatible = "qcom,dwc3";
    dr_mode = "peripheral";
    ...
};
```

🔹 **Fastboot Config (if needed):**

```c
CONFIG_USB_FUNCTION_FASTBOOT=y
CONFIG_CMD_FASTBOOT=y
CONFIG_FASTBOOT_FLASH=y
```

🔹 **Log Example:**

```
Starting USB...
USB0:   USB DWC3
       scanning bus 0 for devices... 1 USB Device(s) found
```

---

### 3. **SPI/NAND Flash**
==============================================================================================================================


🔹 **Why?**
If booting or storing kernel/rootfs on SPI-NOR or NAND.

🔹 **Kconfig:**

```c
CONFIG_SPI=y
CONFIG_DM_SPI=y
CONFIG_SPI_FLASH=y
CONFIG_SPI_FLASH_MTD=y
CONFIG_SPI_FLASH_STMICRO=y  // Example
CONFIG_MTD_DEVICE=y
```

🔹 **Device Tree:**

```dts
spi@78b5000 {
    status = "okay";
    flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        ...
    };
};
```

🔹 **Log Sample:**

```
SF: Detected W25Q128 with page size 256 Bytes, erase size 4 KiB, total 16 MiB
```

---

### 4. **Ethernet (TFTP/NFS)**
==============================================================================================================================


🔹 **Why?**
Useful for development boards or production TFTP/NFS boot.

🔹 **Kconfig:**

```c
CONFIG_NET=y
CONFIG_PHYLIB=y
CONFIG_DM_ETH=y
CONFIG_PHY=y
CONFIG_ETH_DESIGNWARE=y  // example
CONFIG_CMD_NET=y
```

🔹 **Device Tree:**

```dts
ethernet@07870000 {
    compatible = "qcom,dwmac";
    phy-mode = "rgmii";
    ...
};
```

🔹 **Boot Command:**

```bash
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.10
tftpboot 0x80000000 uImage
```

🔹 **Log Sample:**

```
eth0: ethernet@07870000
Using eth0 device
TFTP from server 192.168.1.100; our IP address is 192.168.1.10
Filename 'uImage'.
```

---

### 5. **PMIC / Regulator**
==============================================================================================================================

🔹 **Why?**
Power up other SoCs, storage, memory through voltage rails managed by PMIC.

🔹 **Kconfig:**

```c
CONFIG_POWER=y
CONFIG_POWER_I2C=y
CONFIG_DM_PMIC=y
CONFIG_PMIC_QCOM=y   // PMIC like PM8998
```

🔹 **Device Tree:**

```dts
pmic@0 {
    compatible = "qcom,pm8998";
    ...
};
```

🔹 **Driver Path:**
`drivers/power/pmic/`, `drivers/regulator/`

🔹 **Log Sample:**

```
PMIC: PM8998 regulator initialized
```

---

### 6. **Device Tree for U-Boot**
==============================================================================================================================

🔹 **Options:**

* Use custom U-Boot DTB
* Reuse Linux DTB by trimming OS-specific nodes

🔹 **Location:**

```
arch/arm/dts/<board>.dts
```

🔹 **Config:**

```c
CONFIG_DEFAULT_DEVICE_TREE="sdm660-myboard"
```

🔹 **Build Output:**

```
u-boot.dtb (merged DT + U-Boot config)
```

🔹 **Special Nodes Used by U-Boot:**

* `chosen`
* `memory`
* `stdout-path`

---

### 7. **Board.c (Board-specific Init)**
==============================================================================================================================
🔹 **Why?**
To do pinmuxing, GPIO toggles, peripheral enable before kernel takes over.

🔹 **Typical Tasks:**

* Enable clocks
* Set GPIOs for WiFi/BT
* Configure LEDs, recovery mode
* Detect boot mode (e.g., key press)

🔹 **Path:**

```
board/<vendor>/<board>/board.c
```

🔹 **Example Function:**

```c
int board_init(void)
{
    setup_pmic();
    setup_mmc();
    setup_usb();
    return 0;
}
```

🔹 **Log Sample:**

```
Board: SDM660 EVB
MMC initialized
PMIC voltage rails enabled
```

---

## 🔚 Summary Block Diagram
==============================================================================================================================

```plaintext
             +-------------------+
             |    Power On       |
             +--------+----------+
                      |
             +--------v---------+
             |     ROM Boot     | ← Reads MLO/SPL from eMMC
             +--------+---------+
                      |
             +--------v---------+
             |   SPL (MLO)      |
             | DDR + UART + FS  |
             +--------+---------+
                      |
             +--------v---------+
             |     U-Boot       |
             | Peripheral Init  |
             | MMC/USB/TFTP     |
             | Load Kernel+DTB  |
             +--------+---------+
                      |
             +--------v---------+
             |    Linux Kernel  |
             +------------------+
```

---

Let me know if you want:

* **Real `defconfig`** for U-Boot on SDM660
* **U-Boot `.lds` linker file explained**
* **Sample boot logs**
* **Actual boot commands for MMC/TFTP**
* **Board-specific `board.c` code**

I can walk you through those next.



