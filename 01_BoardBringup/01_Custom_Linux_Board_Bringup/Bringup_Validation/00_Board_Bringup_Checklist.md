
✅ 📋 Custom Linux Board Bring-Up Checklist
=====================================================================================================================================


🔧 1. Pre-Bootloader Setup
==============================================================================================
| Step                       | Description                                                   |
| ---------------------------|---------------------------------------------------------------|
| ✅ Schematic Review        | Verify SoC, power rails, clocks, PMIC, peripherals.           |
| ✅ Clock Tree Plan         | Ensure proper oscillator and PLL routing.                     |
| ✅ DDR Layout + Config     | Confirm memory topology, impedance matching, and termination. |
| ✅ JTAG/UART Access        | Confirm availability for low-level debug.                     |
| ✅ Power-On Reset Sequence | Match with SoC datasheet timing.                              |


🧼 2. Bootloader Bring-Up (SPL/MLO + U-Boot)
=================================================================================
| Step                     | Description                                        |
| -----------------------  | -------------------------------------------------- |
| ✅ SPL (MLO) Porting     | Configure DDR init, stack setup, early UART.       |
| ✅ U-Boot Porting        | Set up MMC, SPI, Ethernet, USB boot paths.         |
| ✅ Console Output        | Confirm UART prints (early boot logs).             |
| ✅ Environment Variables | Customize `bootargs`, `bootcmd`, network settings. |
| ✅ Storage Access        | Enable SD/eMMC/NAND/SPI Flash read/write.          |
| ✅ TFTP/NFS Boot         | Set up U-Boot for TFTP and NFS booting.            |
| ✅ USB Support           | Enable UMS (USB Mass Storage) for easier flashing. |


🐧 3. Linux Kernel Bring-Up
=======================================================================================
| Step                   | Description                                                 |
| ---------------------  | ----------------------------------------------------------- |
| ✅ Kernel Selection    | Use vendor BSP or mainline (preferred if supported).        |
| ✅ Kernel Config       | Use `make <board_defconfig>` then `menuconfig` for drivers. |
| ✅ Enable Early Printk | Use `CONFIG_EARLY_PRINTK` or `earlycon` for debugging.      |
| ✅ Device Tree (.dts)  | Create board-specific DTS, enable UART, MMC, Ethernet, etc. |
| ✅ Kernel Boot         | Ensure kernel boots and mounts rootfs from NFS or SD.       |

📦 4. Root Filesystem (RootFS)
====================================================================================
| Step              | Description                                                  |
| ----------------  | ------------------------------------------------------------ |
| ✅ BusyBox Init   | Use BusyBox or Yocto minimal image for first rootfs.         |
| ✅ Mount Points   | Check `/dev`, `/proc`, `/sys` mounts in `init` or `systemd`. |
| ✅ Serial Console | Ensure `getty` runs on console.                              |
| ✅ Shell Access   | Login prompt with working password and shell.                |
| ✅ Network Stack  | Enable DHCP/static IP, test `ping`, `wget`.                  |

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


🧪 6. Debugging and Validation
============================================================================
| Step                     | Description                                   |
| -------------------------|---------------------------------------------- |
| ✅ `dmesg` Check         | Look for driver probes, errors, IRQ failures. |
| ✅ `strace` or `ltrace`  | Debug failing user applications.              |
| ✅ GPIO/I2C Oscilloscope | Verify signaling integrity and timing.        |
| ✅ Lauterbach/OpenOCD    | Use for low-level tracing via JTAG.           |
| ✅ Kernel Panic Recovery | Test and handle kernel crash scenarios.       |

🔐 7. Security & Production Prep
=======================================================================================
| Step                       | Description                                            |
| ---------------------------|------------------------------------------------------- |
| ✅ Remove Debug Ports      | Disable console, JTAG, unused peripherals.             |
| ✅ Secure Boot             | Sign U-Boot and kernel. Enable signature verification. |
| ✅ Read-only RootFS        | Use SquashFS or overlayfs for root filesystem.         |
| ✅ udev Rules              | Set up persistent device naming.                       |
| ✅ Logging                 | Enable system logs, rotate and store logs.             |
| ✅ Production Flash Layout | Define bootloader, kernel, rootfs partitions clearly.  |

🔄 8. Update & Deployment
======================================================================================
| Step                         | Description                                          |
| ---------------------------  | ---------------------------------------------------- |
| ✅ A/B System or Backup Boot | Add redundancy for kernel/rootfs in case of failure. |
| ✅ OTA (Optional)            | Plan or integrate Over-The-Air update system.        |
| ✅ Recovery Mode             | Setup UMS or DFU mode fallback for rescue.           |


📁 Optional Tools & Documentation
======================================================================================
| Tool                  | Purpose                                               |
| --------------------- | ----------------------------------------------------- |
| Yocto / Buildroot     | Automated build system for full image + SDK.          |
| Git + Patch Workflow  | BSP source versioning and code management.            |
| Wiki / Markdown Docs  | Create internal documentation for steps and configs.  |
| Excel/Checklist Sheet | Track bring-up status of each peripheral & milestone. |








