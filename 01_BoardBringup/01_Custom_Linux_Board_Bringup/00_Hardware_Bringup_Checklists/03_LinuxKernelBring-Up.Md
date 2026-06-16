🐧 3. Linux Kernel Bring-Up
=======================================================================================
| Step                   | Description                                                 |
| ---------------------  | ----------------------------------------------------------- |
| ✅ Kernel Selection    | Use vendor BSP or mainline (preferred if supported).        |
| ✅ Kernel Config       | Use `make <board_defconfig>` then `menuconfig` for drivers. |
| ✅ Enable Early Printk | Use `CONFIG_EARLY_PRINTK` or `earlycon` for debugging.      |
| ✅ Device Tree (.dts)  | Create board-specific DTS, enable UART, MMC, Ethernet, etc. |
| ✅ Kernel Boot         | Ensure kernel boots and mounts rootfs from NFS or SD.       |

✅ 1. Kernel Selection
====================================================================================================================================
Purpose: Choose a Linux kernel that suits your hardware and long-term maintenance needs.

Options:
    **Vendor BSP Kernel**:
        Provided by SoC vendor (e.g., Qualcomm, NXP, TI).
        Includes board-specific patches and hardware support.
        Pros: Quick start, hardware works out-of-box.
        Cons: Often outdated and hard to upstream.
    **Mainline Kernel**:
        Official Linux.org kernel with regular updates.
        Pros: Clean, secure, and actively maintained.
        Cons: Might lack support for new or custom hardware.

Example:
------------
            git clone https://source.codeaurora.org/quic/kernel/skales kernel-msm-4.19

or
---
            git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git


✅ 2. Kernel Configuration
====================================================================================================================================
Purpose: Customize the kernel for your hardware platform.

Steps:
01. Load default config:
                    make <board_defconfig>
Example:
                    make qcom_defconfig

02. Customize using menuconfig:
                    make menuconfig

Use this to:
        Enable drivers: I2C, SPI, USB, Ethernet, etc.
        Enable filesystems: EXT4, NFS, etc.
        Enable kernel debugging features.
        Set root filesystem path (NFS/SD card).

Tip:
        Use make savedefconfig to export only the differences from defconfig.

✅ 3. Enable Early Printk
====================================================================================================================================
Purpose: Get early debug logs before full console driver is initialized.

Methods:
----------

**Kernel Config:**
    Enable:
            CONFIG_EARLY_PRINTK=y
            CONFIG_DEBUG_LL=y
            CONFIG_DEBUG_UART_PHYS=0xXXXXXXXX  # UART base addr
            CONFIG_DEBUG_UART_VIRT=0xXXXXXXXX
**Bootargs (U-Boot):**
    bash:
            setenv bootargs "earlycon=uart8250,mmio32,0xXXXXXXXX console=ttyS0,115200"

Why?
--------
        Early printk helps see panic messages, memory map info, or errors before device drivers are ready


✅ 4. Device Tree (.dts)
====================================================================================================================================
Purpose: Tell the kernel about the hardware (since it no longer hardcodes platform info).

Steps:
**01. Create DTS for your custom board under:**
    arch/arm/boot/dts/ or arch/arm64/boot/dts/

**02. Define peripherals:**
    Enable UART, I2C, SPI, Ethernet, MMC, regulators, clocks, etc.

dts

    &uart2 {
        status = "okay";
    };
    
    &mmc1 {
        bus-width = <4>;
        status = "okay";
    };

**03. Build DTB:**
    bash
        make dtbs

**04. Pass DTB from bootloader:**

    U-Boot: bootm <kernel> - <dtb>


Tip:
    Always verify device nodes are mapped to correct base addresses and IRQs.

✅ 5. Kernel Boot
====================================================================================================================================
Purpose: Get the kernel to boot successfully and reach an operational userspace.

**01. Checklist:**
    Ensure correct bootargs:
                            setenv bootargs "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait rw"

**02. Check for root filesystem options:**
SD card: 
    Use ext4, vfat, or similar partition with /sbin/init.
NFS Rootfs:
    Kernel must support CONFIG_NFS_FS.
    Add bootarg:
        bash
            root=/dev/nfs nfsroot=<host_ip>:/nfs/rootfs ip=dhcp

**03. Confirm:**
    Kernel mounts the rootfs.
    Init starts.
    You get a login prompt or shell.

🧠 Summary Flow Diagram:
====================================================================================================================================

              +---------------------+
              |  U-Boot (Bootloader)|
              +----------+----------+
                         |
                         v
             +-----------+-----------+
             |     Kernel Image      |
             | (with DTB or passed)  |
             +-----------+-----------+
                         |
         +---------------+----------------+
         | Kernel Init (MMU, earlycon, ...)|
         +---------------+----------------+
                         |
               +---------v--------+
               | Device Tree (DTS)|
               | - Enable UART    |
               | - Enable MMC/NFS |
               +---------+--------+
                         |
               +---------v--------+
               | Mount Root Filesystem |
               | /sbin/init or systemd |
               +-----------------------+



Here’s a realistic Linux kernel boot log snippet (from an embedded ARM board like SDM660 or i.MX6), along with line-by-line explanation so you understand what happens during each stage of bring-up.
====================================================================================================================================

**🧾 Boot Log Snippet (UART Console Output)**

U-Boot 2018.03 (Apr 01 2025 - 10:00:00 +0530)

DRAM:  2 GiB
MMC:   sdhci@7824900: 0
Loading Environment from MMC... OK
Booting Linux...

## Loading kernel from FIT Image at 0x82000000 ...
   Using 'conf@1' configuration
   Verifying Hash Integrity ... OK
   Loading Kernel Image ... OK
   Loading Device Tree to 00000000f5ef0000, size 0x00010000 ... OK

Starting kernel ...

[    0.000000] Booting Linux on physical CPU 0x0
[    0.000000] Linux version 5.10.0 (gcc version 9.3.0) #1 SMP PREEMPT
[    0.000000] Machine model: Custom SDM660 Dev Board
[    0.000000] earlycon: msm_serial0 at MMIO 0x78af000
[    0.000000] printk: bootconsole [msm_serial0] enabled
[    0.000000] CPU: ARMv8 Processor [410fd034] revision 4
[    0.000000] efi: UEFI not found.
[    0.000000] Zone ranges:
[    0.000000]   DMA      [mem 0x0000000080000000-0x00000000ffffffff]
[    0.000000]   Normal   [mem 0x0000000100000000-0x000000017fffffff]
[    0.000000] psci: probing for conduit method from DT.
[    0.000000] psci: Using standard PSCI v0.2 function IDs
[    0.000000] percpu: Embedded 20 pages/cpu s42968 r8192 d29704 u81920
[    0.000000] Built 1 zonelists, mobility grouping on.  Total pages: 520192

[    0.010000] CPU1: Booted secondary processor 0x0000000001 [0x410fd034]
[    0.020000] Detected SDM660 SoC
[    0.030000] smp: Bringing up secondary CPUs ...
[    0.035000] CPU2: Booted secondary processor 0x0000000002
[    0.040000] CPU3: Booted secondary processor 0x0000000003
[    0.050000] devtmpfs: initialized
[    0.060000] Registered cp15_barrier emulation handler
[    0.070000] hw-breakpoint: found 6 breakpoint and 4 watchpoint registers.
[    0.080000] Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled
[    0.090000] msm_serial 78af000.serial: ttyMSM0 at MMIO 0x78af000 (irq = 33, base_baud = 460800) is a MSM
[    0.100000] printk: console [ttyMSM0] enabled
[    0.110000] loop: module loaded
[    0.120000] mmc0: SDHCI controller on 7824900.sdhci [7824900.sdhci] using ADMA
[    0.130000] mmc0: new high speed SDHC card at address 0001
[    0.140000] mmcblk0: mmc0:0001 SD16G 14.4 GiB 
[    0.150000]  mmcblk0: p1 p2
[    0.160000] EXT4-fs (mmcblk0p2): mounted filesystem with ordered data mode. Opts: (null)
[    0.170000] VFS: Mounted root (ext4 filesystem) on device 179:2.
[    0.180000] Freeing unused kernel memory: 1024K
[    0.190000] Run /sbin/init as init process
[    0.200000] systemd[1]: System initialization complete.


====================================================================================================================================
