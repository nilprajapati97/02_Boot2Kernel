🐧 3. Linux Kernel Bring-Up
=======================================================================================
| Step                   | Description                                                 |
| ---------------------  | ----------------------------------------------------------- |
| ✅ Kernel Selection    | Use vendor BSP or mainline (preferred if supported).        |
| ✅ Kernel Config       | Use `make <board_defconfig>` then `menuconfig` for drivers. |
| ✅ Enable Early Printk | Use `CONFIG_EARLY_PRINTK` or `earlycon` for debugging.      |
| ✅ Device Tree (.dts)  | Create board-specific DTS, enable UART, MMC, Ethernet, etc. |
| ✅ Kernel Boot         | Ensure kernel boots and mounts rootfs from NFS or SD.       |