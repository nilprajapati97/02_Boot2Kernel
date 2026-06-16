Here’s a **deep, from-scratch explanation of SPL (MLO) Porting** for your custom Embedded Linux board (like AM335x, i.MX, SDM660, etc.).

---

## ✅ 1. WHAT IS SPL (MLO)?

**SPL = Secondary Program Loader**
**MLO** = The actual binary name used for SPL on TI SoCs

🔸 SPL is a **minimal first-stage bootloader** that:

* Runs in **limited internal SRAM**
* Initializes essential hardware (DDR, clocks, UART)
* Loads **U-Boot proper** into DRAM and jumps to it

### 🧠 Why SPL?

Because U-Boot is too big to fit in SRAM, SPL does only minimal init and leaves the heavy lifting to U-Boot.

---

## ✅ 2. SPL PORTING GOALS

Your job when porting SPL:

| Objective           | Result                                                       |
| ------------------- | ------------------------------------------------------------ |
| Initialize hardware | Clock, UART, DDR so U-Boot runs correctly                    |
| Fit in SRAM         | SPL size must be small (`CONFIG_SPL_*` features are limited) |
| Setup boot source   | SPL must read U-Boot from eMMC, SD, QSPI, etc.               |
| Ensure early UART   | Debug prints as early as possible                            |

---

## ✅ 3. SPL PORTING COMPONENTS (IN DEPTH)

---

### 🔹 A. DDR INIT

#### ❓ Why?

SPL must initialize **DRAM controller** before it can load full U-Boot into RAM.

#### 🔧 Where?

* `board/<vendor>/<board>/spl.c`
* SoC-specific: `arch/arm/mach-<arch>/`

#### 💡 Options:

* **Use vendor code/blobs** (e.g. TI's EMIF init)
* **Use `Kconfig` options**: `CONFIG_SPL_DRAM_INIT`, `CONFIG_SYS_SDRAM_BASE`

#### Example:

```c
void spl_board_init(void)
{
    // Early init
    enable_uart();
    setup_clocks();
    ddr_init();
}
```

#### 🔍 Debug logs (if early UART is enabled):

```
[ SPL ] DDR init start...
[ SPL ] DDR size = 512MB
```

---

### 🔹 B. EARLY CLOCK SETUP

#### ❓ Why?

* Needed to drive UART, MMC, DRAM controller
* Usually involves setting PLLs

#### 🔧 Where?

* `clock.c`, `pll.c`, or `spl.c` in SoC directory

#### Example:

```c
void setup_clocks(void)
{
    configure_main_pll();
    enable_peripheral_clocks();
}
```

---

### 🔹 C. EARLY UART SETUP

#### ❓ Why?

* For seeing debug prints even before DRAM works

#### ✅ U-Boot Config:

```c
CONFIG_DEBUG_UART=y
CONFIG_DEBUG_UART_OMAP=y         // for TI
CONFIG_DEBUG_UART_BASE=0x44e09000
CONFIG_DEBUG_UART_CLOCK=48000000
CONFIG_DEBUG_UART_SHIFT=2
```

#### File to edit: `include/debug_uart.h`

#### Log:

```
[ SPL ] Early UART active...
```

---

### 🔹 D. STACK SETUP

#### ❓ Why?

* Code needs stack to operate
* Place in **on-chip SRAM** OR **DRAM** after it's initialized

#### Address depends on SoC:

* On-chip SRAM: 0x402F0400 (TI AM335x)
* DRAM (after init): 0x80000000 +

#### Code:

```c
#define CONFIG_SPL_STACK    0x402F0400
```

Set in:

* `include/configs/<board>.h`
* Or `Kconfig` for SPL

---

### 🔹 E. BOOT SOURCE INIT (MMC, QSPI, NAND, etc.)

#### ❓ Why?

SPL must **load U-Boot.img** from boot source

#### Config:

```c
CONFIG_SPL_MMC_SUPPORT=y
CONFIG_SPL_SPI_SUPPORT=y
CONFIG_SPL_NAND_SUPPORT=y
```

#### Example (MMC):

```c
int spl_mmc_load_image(void)
{
    mmc_initialize();
    mmc_read(U_BOOT_SECTOR, &u_boot_buf, size);
}
```

🔍 U-Boot log:

```
SPL: Booting from MMC
```

---

### 🔹 F. SPL TUNING: Reduce Image Size

#### ❓ Why?

* SPL must fit in **SoC's internal SRAM**
* Remove unnecessary features

#### Use these flags:

```c
CONFIG_SPL=y
CONFIG_SPL_BOARD_INIT=y
CONFIG_SPL_YMODEM_SUPPORT=n
CONFIG_SPL_USB_SUPPORT=n
CONFIG_SPL_FAT_SUPPORT=y
```

Also:

```c
CONFIG_SPL_TEXT_BASE=0x402F0400     // SRAM base
CONFIG_SPL_MAX_SIZE=0x20000         // SRAM limit
```

---

## ✅ 4. SPL FILE STRUCTURE IN U-BOOT

| File                           | Role                         |
| ------------------------------ | ---------------------------- |
| `spl/u-boot-spl`               | Raw SPL binary               |
| `MLO`                          | TI-formatted SPL             |
| `board/<vendor>/<board>/spl.c` | DDR init, clocks, UART setup |
| `arch/arm/mach-*/`             | SoC support code             |
| `include/configs/<board>.h`    | SPL stack, base address      |

---

## ✅ 5. UART LOGS FROM SPL (First Success)

```sh
U-Boot SPL 2025.07 (Jul 26 2025 - 10:00:00)
[ SPL ] PLL setup done
[ SPL ] DDR init: OK
[ SPL ] Stack at 0x402F0400
[ SPL ] Trying to boot from MMC1
[ SPL ] U-Boot loaded at 0x80000000
```

---

## ✅ 6. DEBUGGING TIPS

| Symptom                 | Check This                             |
| ----------------------- | -------------------------------------- |
| No UART output          | DEBUG\_UART config, clock, pinmux      |
| SPL hangs after UART    | DDR init issue                         |
| SPL loads but no U-Boot | Wrong U-Boot offset, wrong boot medium |
| SPL too large           | Disable features, check `SPL_MAX_SIZE` |

---

## ✅ 7. TEST YOUR SPL

### Build:

```bash
make <board>_defconfig
make -j$(nproc)
```

### Flash (SD boot):

```bash
sudo dd if=MLO of=/dev/sdX bs=512 seek=1
sudo dd if=u-boot.img of=/dev/sdX bs=512 seek=96
```

---

## ✅ 8. EXAMPLE FULL CONFIG SNIPPET

```c
#define CONFIG_SPL_TEXT_BASE        0x402F0400
#define CONFIG_SPL_STACK            0x4030B800
#define CONFIG_SYS_SPL_MALLOC_START 0x80208000
#define CONFIG_SYS_SPL_MALLOC_SIZE  0x100000

#define CONFIG_SPL_UART_BASE        0x44e09000
#define CONFIG_SPL_BSS_START_ADDR   0x80000000
#define CONFIG_SPL_BSS_MAX_SIZE     0x80000
```

---

## ✅ FINAL CHECKLIST FOR SPL BRING-UP

| ✔️ Item                | Status         |
| ---------------------- | -------------- |
| UART works             | ✅ Debug prints |
| Clocks initialized     | ✅              |
| DDR initialized        | ✅ Tested       |
| Stack configured       | ✅ SRAM OK      |
| MMC/QSPI works         | ✅              |
| SPL size < SRAM limit  | ✅              |
| U-Boot loaded into RAM | ✅              |

---

Would you like a **real porting template** for a board like `am335x`, `imx6`, or `sdm660`?
I can provide full code + defconfig + MLO layout per SoC.
