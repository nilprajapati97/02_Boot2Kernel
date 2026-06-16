Flashing **U-Boot/SPL** (Secondary Program Loader / 1st stage bootloader) via **JTAG** or **UART** is essential during **initial board bring-up**, **bootloader recovery**, or **manufacturing**—especially when the storage (eMMC/SD/NAND) isn't accessible.

---

# 🚀 Flashing U-Boot & SPL via JTAG or UART – In-Depth

---

## 🧠 Bootloader Stages Recap (ARM SoCs)

```plaintext
[ Boot ROM (SoC Internal) ]
         ↓
[ SPL / MLO (Stage 1) ]
         ↓
[ U-Boot (Stage 2) ]
         ↓
[ Kernel → DTB → RootFS ]
```

If SPL/U-Boot is corrupted or not yet flashed, JTAG or UART can be used to load and write it to flash.

---

## 🧰 Methods to Flash U-Boot/SPL

| Interface | Use Case                     | Speed | Tools Required            |
| --------- | ---------------------------- | ----- | ------------------------- |
| JTAG      | Early bring-up, full control | Fast  | OpenOCD, Lauterbach, etc. |
| UART      | SoC supports boot via UART   | Slow  | XMODEM/USB-UART, TeraTerm |

---

## 🔌 A. Flashing U-Boot/SPL Using **JTAG**

### 🔧 Requirements:

* JTAG debugger (e.g., Segger J-Link, Lauterbach, Flyswatter, ARM DAPLink)
* OpenOCD or Lauterbach Trace32 software
* Board with accessible JTAG header (10-pin/20-pin ARM layout)
* U-Boot binaries (`SPL`, `u-boot.img`)

---

### ⚙️ Steps (Using OpenOCD)

#### 1. **Connect JTAG**

* Ensure correct **pinout** (TDI, TDO, TMS, TCK, TRST, nRESET, GND)
* Power on board

#### 2. **Start OpenOCD**

Example for i.MX6:

```bash
openocd -f interface/jlink.cfg -f target/imx6.cfg
```

#### 3. **Open telnet or GDB session**

```bash
telnet localhost 4444
```

#### 4. **Load SPL/U-Boot into RAM**

```bash
load_image SPL 0x40200000 bin
load_image u-boot.img 0x80800000 bin
```

#### 5. **Write to flash (e.g., NAND or eMMC)**

```bash
# Erase flash manually
nand probe 0
nand erase 0 0x80000

# Write SPL to NAND
nand write 0x40200000 0 0x80000

# Write U-Boot
nand write 0x80800000 0x80000 0x100000
```

#### 6. **Reset and boot**

```bash
reset run
```

---

### ✅ Notes:

* You can also script this in `.cfg` or via GDB using `load` and `mon` commands.
* Useful when NAND is corrupted or bootloader is missing.

---

## 🔌 B. Flashing U-Boot/SPL Using **UART**

Many SoCs (TI, NXP, Allwinner) support **booting over UART** as a fallback or recovery method.

---

### 🔧 Requirements:

* USB-to-Serial cable (FTDI/CP210x)
* UART boot mode enabled on SoC (via boot switches/jumpers)
* Terminal tool: `TeraTerm`, `minicom`, `kermit`
* SPL and U-Boot binaries
* `XMODEM`, `YMODEM`, or custom UART boot tool (e.g., `imx_usb_loader`, `sunxi-fel`, `k3r5load`)

---

### ⚙️ UART Booting Procedure (TI AM335x example)

#### 1. **Set Boot Mode to UART**

* Change boot mode pins (BOOT\[2:0]) to UART.
* Power cycle board.

#### 2. **SoC waits for binary over UART**

Use `kermit` or `YMODEM` to send SPL:

```bash
sx -X MLO < /dev/ttyUSB0 > /dev/ttyUSB0
```

Or use vendor tools:

* TI: `sfh_OMAP`, `uniflash`
* NXP: `imx_usb_loader`
* Allwinner: `sunxi-fel`

#### 3. **SPL runs and loads U-Boot**

* Once SPL is running, it configures DRAM and loads `u-boot.img`
* Send `u-boot.img` over UART using YMODEM again

#### 4. **U-Boot Shell Available**

You can now flash permanent images from U-Boot prompt:

```bash
loady 0x82000000      # Receive new U-Boot via YMODEM
sf probe 0            # SPI NOR/NAND init
sf erase 0 100000
sf write 0x82000000 0 100000
```

---

### 🧠 Common U-Boot Flashing Commands

| Command          | Purpose                |
| ---------------- | ---------------------- |
| `loady`          | Load via YMODEM (UART) |
| `loadx`          | Load via XMODEM        |
| `loadb`          | Load via Kermit        |
| `sf`             | SPI flash ops          |
| `mmc`            | SD/eMMC ops            |
| `nand`           | NAND flash ops         |
| `env default -a` | Reset U-Boot env       |
| `saveenv`        | Save environment       |

---

## 📦 Example: Full Recovery via UART (TI AM335x)

```bash
# Step 1: Send MLO (SPL) over YMODEM
sx MLO < /dev/ttyUSB0 > /dev/ttyUSB0

# Step 2: SPL asks for u-boot.img, send via YMODEM
sx u-boot.img < /dev/ttyUSB0 > /dev/ttyUSB0

# Step 3: Flash to eMMC
mmc dev 0
fatload mmc 0:1 0x82000000 u-boot.img
mmc write 0x82000000 0x300 0x400
```

---

## 🛠️ Troubleshooting

| Issue                   | Reason                         | Fix                                   |
| ----------------------- | ------------------------------ | ------------------------------------- |
| No output over UART     | Wrong port, cable, or bootmode | Check serial connection and boot pins |
| SPL runs but fails DRAM | Bad DRAM init in code          | Check DDR settings in SPL config      |
| Flash command fails     | Flash not probed or erased     | Use `sf probe`, `nand erase`          |
| `loady` hangs           | Wrong protocol or size         | Use `sx`, match memory address        |

---

## ✅ Summary: Choosing Between JTAG vs UART

| Feature        | JTAG                        | UART                           |
| -------------- | --------------------------- | ------------------------------ |
| Speed          | Fast (RAM direct access)    | Slow (typically 115200 baud)   |
| Control        | Full (RAM, flash, CPU regs) | Limited to bootloader loading  |
| Use case       | Early bring-up, debug       | Recovery, low-cost programming |
| Tools required | Hardware debugger + OpenOCD | USB-Serial + terminal tools    |

---

Let me know if you'd like:

* OpenOCD script for a specific SoC (i.MX, STM32, etc.)
* UART boot config for a particular board
* Flash map + partition layout (DTS) walkthrough

I'll tailor it to your exact board/SOC!
