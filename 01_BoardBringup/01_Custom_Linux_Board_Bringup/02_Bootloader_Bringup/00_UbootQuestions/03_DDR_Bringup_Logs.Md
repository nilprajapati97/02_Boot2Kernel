Here's a **realistic UART log example** during **DDR initialization in SPL** — useful for debugging and understanding the **boot sequence** from early SPL to U-Boot proper.

---

## ✅ Example: DDR Bring-Up Logs (UART via SPL)

This is from a typical **TI AM335x or i.MX6** based board using U-Boot SPL, booting from SD card.

---

### 🔌 **Power-On → SPL Execution**

```
U-Boot SPL 2023.01 (Aug 02 2025 - 10:12:47 +0530)
Trying to boot from MMC1
```

> ✅ **SPL starts executing from SRAM. It detected SD/MMC as the boot source.**

---

### 🧠 **Clock & Pinmux Init**

```
Initializing PRCM...
Configuring pinmux...
```

> ✅ SPL configures clocks, PLLs, and pads — critical before accessing peripherals like UART or DDR.

---

### 💾 **DDR Initialization Begins**

```
Starting DDR initialization...
Using predefined DDR3 settings for 512MB
EMIF: SDRAM_CONFIG   = 0x61C05332
EMIF: REF_CTRL       = 0x00000C30
EMIF: SDRAM_TIM_1    = 0x0AAAD4DB
EMIF: ZQ_CONFIG      = 0x50074BE4
DDR PHY calibrated
DDR initialized successfully
```

> ✅ These logs come from `sdram_init()` or `config_ddr()` after writing to memory controller and PHY.

> DDR is now usable; SPL can load larger binaries like U-Boot proper into DDR memory.

---

### 📥 **U-Boot Proper Loading**

```
Loading U-Boot from MMC...
Reading image from 0x00080000
U-Boot loaded at: 0x80800000
Jumping to U-Boot...
```

> ✅ SPL finishes its job by reading `u-boot.img` and jumping to it.

---

### 🐧 **U-Boot Proper Starts**

```
U-Boot 2023.01 (Aug 02 2025 - 10:12:47 +0530)

CPU  : AM335x-GP rev 2.1
Model: Custom Board X1 RevC
DRAM:  512 MiB
MMC:   OMAP SD/MMC: 0
Net:   cpsw
Hit any key to stop autoboot:  0 
```

> ✅ DDR is fully functional. U-Boot is running from DDR. You can now boot kernel or run commands.

---

## ✅ What These Logs Prove

| Log Line                       | What it Confirms                                     |
| ------------------------------ | ---------------------------------------------------- |
| `U-Boot SPL`                   | SPL is running from SRAM                             |
| `DDR initialized successfully` | DDR controller + PHY + timing is working             |
| `Loading U-Boot from MMC`      | SPL can access storage (FAT/NAND/eMMC)               |
| `Jumping to U-Boot`            | U-Boot was loaded into DDR at `CONFIG_SYS_TEXT_BASE` |
| `DRAM: 512 MiB`                | U-Boot's RAM detection (via board/SoC code) is valid |

---

## 🧪 Tips for Debugging If DDR Init Fails

| Symptom                               | Likely Cause                                     |
| ------------------------------------- | ------------------------------------------------ |
| **No UART output at all**             | SPL not even running; BootROM or SD load failure |
| **SPL starts, but hangs at DDR init** | Wrong DDR config (timing, voltage, impedance)    |
| **Garbage characters on UART**        | Incorrect UART clock or pinmux                   |
| **"Failed to calibrate PHY"**         | DDR PHY or IO delay settings are incorrect       |

You can add additional debug logs in `config_ddr()` and `board_init_f()` to pinpoint the failure.

---

Would you like:

* A **minimal SPL with only UART + DDR init** code example?
* How to tune and validate DDR timings with manufacturer datasheet?
* JTAG method for confirming DDR controller status?
