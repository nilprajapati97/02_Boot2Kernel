🔥 How U-Boot Works (in-depth for embedded C++ developers)
=======================================================================
U-Boot (short for Universal Bootloader) is an open-source bootloader used in embedded systems. It initializes the hardware, loads the kernel (or another image like a DTB or rootfs), and hands over control to the operating system (like Linux, QNX, RTOS).

🧱 U-Boot Architecture Overview
=======================================================================
+----------------------------+
| ROM Code / SPL / TF-A     |  ← BootROM (SoC vendor boot code)
+----------------------------+
| Secondary Program Loader   |  ← (Optional) SPL (e.g., for DDR init)
+----------------------------+
| U-Boot Proper (u-boot.img)|
| + CLI / bootcmd support   |
+----------------------------+
| Linux Kernel / OS / App   |  ← Loaded and executed by U-Boot
+----------------------------+


🧠 Key Stages in U-Boot Boot Process
=======================================================================
🔹 1. Boot ROM / Preloader (executed by CPU on power-up)
---------------------------------------------------------------
Embedded in SoC (non-modifiable)

Initializes minimal peripherals (e.g., UART, NAND)

Loads SPL or full U-Boot into SRAM or DDR

🔹 2. SPL (Secondary Program Loader) (Optional)
---------------------------------------------------------------
Used when full U-Boot can't fit into SRAM

Minimal U-Boot version: only necessary drivers (DDR init, NAND/SPI load, UART)

Loads full U-Boot Proper into DDR

Example in SPL:


board_init_f(); // basic init: clocks, timers
dram_init();    // DDR memory controller setup

🔹 3. U-Boot Proper
---------------------------------------------------------------
Full-featured bootloader in DDR

Loads OS kernel, DTB, initrd, etc.

Provides interactive CLI (via UART/USB)

Supports scripting via bootcmd, TFTP, MMC, NAND, SPI



Boot sequence:
=======================================================================
board_init_r();       // full board init (console, storage)
run_preboot_environment(); // run boot scripts
autoboot_command();   // run bootcmd or wait for CLI input
🛠 What Does U-Boot Do?
Feature	Description
Memory Init	Initializes DDR, stack, BSS segment
Console I/O	UART for CLI/debug
Flash Access	Reads/writes NAND, eMMC, QSPI, NOR
Peripheral Init	SPI, I2C, USB, Ethernet
Boot Methods	MMC, TFTP, USB, PXE, FIT image
OS Loading	Loads Linux kernel (zImage, Image, uImage)
DTB Support	Loads flattened device tree
Environment	bootargs, bootcmd stored in flash
Secure Boot	Verified boot via RSA keys (if enabled)

🧪 Example U-Boot Environment
=======================================================================

=> printenv
bootcmd=run mmcboot
bootargs=console=ttyS0,115200 root=/dev/mmcblk0p2 rw
mmcboot=load mmc 0:1 0x80000 zImage; bootz 0x80000 - 0x100000
Flow:
bootcmd calls mmcboot

mmcboot loads zImage to address 0x80000

bootz jumps to kernel

💡 Typical Memory Layout (ARM)
=======================================================================

0x40000000 ← DDR start
0x42000000 ← U-Boot Proper
0x43000000 ← zImage
0x44000000 ← DTB
0x45000000 ← initrd

🔐 Secure Boot with U-Boot
---------------------------------------------------------------
Integrates RSA / SHA verification
Verifies signed U-Boot, kernel, DTB before execution
Uses keys burned in OTP or secure flash

⚙️ U-Boot Build and Flashing (Workflow)
=======================================================================

make <board_defconfig>      # Select board
make                         # Build SPL + U-Boot
Image files:

spl/u-boot-spl.bin
u-boot.img
u-boot.dtb (optional for DTB support)

Flash to:
   eMMC, NAND, QSPI, SD card (depends on SoC boot mode)

🔄 Boot Flow Recap
=======================================================================

[Power ON]
   ↓
[BootROM (ROM code)]
   ↓
[Load SPL from flash]
   ↓
[SPL → init DDR, UART]
   ↓
[Load U-Boot Proper]
   ↓
[U-Boot → CLI or bootcmd]
   ↓
[Load zImage + DTB + initrd]
   ↓
[Jump to kernel entry point]
🧰 Tools and Commands
=======================================================================
Tool	            Use
mkimage	            Create uImage or FIT files
bootz, bootm	      Boot zImage, uImage
load mmc, tftpboot	Load binaries from SD/TFTP
saveenv, setenv	   Save/edit environment variables
bdinfo, printenv	   Show board/device info

🧩 Real-World Use Cases
=======================================================================
Android bootloaders (early stage → handoff to ABL)
RTOS boot for industrial controllers
IoT devices booting from SPI or QSPI flash
UEFI/GRUB replacement on ARM



                    +-----------------------------+
                    |       Power-On / Reset      |
                    +-------------+---------------+
                                  |
                                  v
                    +-----------------------------+
                    |     SoC BootROM Code         |
                    |  - Fixed in hardware         |
                    |  - Initializes basic IO      |
                    |  - Loads SPL from flash      |
                    +-------------+---------------+
                                  |
                                  v
          +----------------------------------------------+
          |         SPL (Secondary Program Loader)        |
          |  - Tiny bootloader (~32KB)                    |
          |  - Initializes DDR (RAM)                      |
          |  - Initializes UART (optional)                |
          |  - Loads U-Boot proper into DDR               |
          +------------------+---------------------------+
                             |
                             v
         +---------------------------------------------+
         |           U-Boot Proper (u-boot.img)         |
         |  - Full bootloader with CLI                  |
         |  - Initializes full board peripherals        |
         |  - Parses environment variables              |
         |  - Executes bootcmd script                   |
         +------------------+--------------------------+
                            |
                            v
     +----------------------------------------------------+
     |         Load Kernel + DTB + Initrd (if any)         |
     |  - From MMC, NAND, USB, TFTP, etc.                  |
     |  - zImage/uImage is loaded to memory                |
     |  - Device tree (DTB) is passed to kernel            |
     +------------------+---------------------------------+
                         |
                         v
        +---------------------------------------------+
        |          Jump to Linux Kernel Entry          |
        |  - U-Boot gives control to kernel            |
        |  - Kernel initializes user space (init/systemd) |
        +---------------------------------------------+


✅ Component Summary
Stage	            Description	            Purpose
-------------------------------------------------
BootROM	         Internal SoC code	    Load SPL or U-Boot
SPL	            Minimal U-Boot	       RAM init + load U-Boot
U-Boot Proper	   Full bootloader	    Load kernel + dtb + initrd
Kernel	         Operating System	    Boot Linux, QNX, RTOS



✅ Common Sources:
==================================================
Stage	               Load From	        Format
--------------------------------------------------
SPL	            NAND / eMMC / QSPI	 u-boot-spl.bin
U-Boot Proper	   DDR	                u-boot.img
Kernel	         MMC / TFTP / USB	    zImage, Image, uImage
DTB	            Same as kernel	       .dtb
Initrd	         Optional	             .cpio, .gz



How to modify and recompile U-Boot for a custom board?
================================================================


✅ 1. Get U-Boot Source Code
-----------------------------------------------------------------
Clone the official U-Boot repository:


git clone https://source.denx.de/u-boot/u-boot.git
cd u-boot
Use a stable tag:


git checkout v2024.01
✅ 2. Toolchain Setup
-----------------------------------------------------------------
Use the cross-compiler matching your board architecture:

Target	         Toolchain Example
------------------------------------------
ARM (32-bit)	   arm-linux-gnueabihf-
ARM64 (AArch64)	aarch64-linux-gnu-
RISC-V	         riscv64-unknown-linux-gnu-

Example (ARM 32-bit):
                     export CROSS_COMPILE=arm-linux-gnueabihf-

✅ 3. Start from a Similar Board
-----------------------------------------------------------------
Use an existing board that's close to your hardware (SoC family, memory map, etc.).

List available boards:


ls configs | grep <SoC or vendor>
Example:
      make am335x_evm_defconfig

✅ 4. Create Your Custom Board Support Package (BSP)
-----------------------------------------------------------------
📁 Create Directories and Files:
                              board/mycompany/myboard/
                              include/configs/myboard.h
                              configs/myboard_defconfig
✅ 5. Edit myboard_defconfig
-----------------------------------------------------------------
Minimal example:
                     CONFIG_ARM=y
                     CONFIG_ARCH_SUNXI=y         # Replace with your SoC
                     CONFIG_MACH_MYBOARD=y       # Your board name
                     CONFIG_SYS_CONFIG_NAME="myboard"

✅ 6. Edit Device Tree (.dts)
-----------------------------------------------------------------
Create a custom DT:

                  arch/arm/dts/myboard.dts
                  Include common SoC file:

#include "am335x.dtsi"
/ {
    model = "My Custom Board";
    ...
};
Update Makefile:

dtb-$(CONFIG_MYBOARD) += myboard.dtb

✅ 7. Edit Board Init Code
-----------------------------------------------------------------
Create files:
               board/mycompany/myboard/myboard.c
Implement functions like:
                        int board_init(void) {
                            // Init pinmux, GPIOs, etc.
                            return 0;
                        }

int dram_init(void) {
    gd->ram_size = <DDR_SIZE>;
    return 0;
}

✅ 8. Update U-Boot Kconfig
-----------------------------------------------------------------
To expose your board to make:
Add config MYBOARD in Kconfig
Enable your board config in boards.cfg or Kconfig/Makefile

✅ 9. Build U-Boot
-----------------------------------------------------------------

make myboard_defconfig
make -j$(nproc)
✅ You’ll get:


u-boot.img – main bootloader
u-boot-spl.bin – if SPL used
myboard.dtb – your device tree

✅ 10. Flash to Target Device
-----------------------------------------------------------------
Depending on your boot source:
SD Card (RAW):
               dd if=u-boot-spl.bin of=/dev/sdX bs=1K seek=1
               dd if=u-boot.img of=/dev/sdX bs=1K seek=69
               eMMC/NAND/QSPI: Use vendor tools or dfu-util, tftpboot, etc.

✅ Tips for Real-World Porting
-----------------------------------------------------------------
Area	What to Focus On
Clock Init	Configure PLL, system clocks early
DDR Init	Setup via code or SPL
UART	For early console/log debug
PMIC/I2C	Bring up power rails, voltage domains
Pinmux	Ensure pins are set to correct function
Watchdog	Disable if interfering with boot

✅ Example Directory Structure
-----------------------------------------------------------------

board/mycompany/myboard/
├── Makefile
├── myboard.c
include/configs/
└── myboard.h
configs/
└── myboard_defconfig
arch/arm/dts/
└── myboard.dts
✅ Test Commands in U-Boot
-----------------------------------------------------------------
Once built and running on target:


printenv              # View environment
load mmc 0:1 0x80000000 zImage
bootz 0x80000000 - 0x81000000
