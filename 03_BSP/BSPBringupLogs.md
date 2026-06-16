Here's a real-world style **specific BSP bring-up log** example showing **U-Boot + Linux kernel boot** on an embedded board (e.g., ARM Cortex-A9 SoC like i.MX6 or similar). This log is extremely useful to understand the **BSP flow** from power-up to Linux shell or kernel panic (if no rootfs).

---

## 📜 **Example: BSP Bring-Up Logs** — U-Boot + Linux

### ✅ **Boot Flow:**

```
SPL → U-Boot → Device Tree (DTB) → Kernel Image (zImage/uImage) → UART Console → [Init or Panic]
```

---

## 🔹 \[1] SPL Log (Secondary Program Loader)

```text
U-Boot SPL 2021.10 (Aug 05 2025 - 10:25:03 +0530)
Trying to boot from MMC1
spl: mmc init succeeded
spl: loading u-boot.img from mmc...
reading u-boot.img
U-Boot loaded from MMC, jumping to U-Boot...
```

---

## 🔹 \[2] U-Boot Log

```text
U-Boot 2021.10 (Aug 05 2025 - 10:25:03 +0530) for MY_CUSTOM_BOARD

CPU:   i.MX6 DualLite/Solo rev1.2 at 792 MHz
Reset cause: POR
DRAM:  512 MiB
MMC:   FSL_SDHC: 0
Video: No display configured
In:    serial
Out:   serial
Err:   serial

Net:   eth0: FEC
Hit any key to stop autoboot:  0

=> printenv
bootcmd=load mmc 0:1 ${kernel_addr} zImage; load mmc 0:1 ${fdt_addr} myboard.dtb; bootz ${kernel_addr} - ${fdt_addr}
bootargs=console=ttymxc0,115200 root=/dev/mmcblk0p2 rw rootwait

=> run bootcmd
```

---

## 🔹 \[3] Kernel Boot Log (Early Printk Enabled)

```text
Booting Linux on physical CPU 0x0
Linux version 5.10.198 (anil@devhost) (arm-linux-gnueabihf-gcc (Linaro GCC 9.3.0)) #1 SMP Fri Aug 5 10:26:32 IST 2025
CPU: ARMv7 Processor [412fc09a] revision 10 (ARMv7), cr=10c5387d
CPU: VIPT nonaliasing data cache, VIPT aliasing instruction cache
Machine model: My Custom Board
Memory policy: Data cache writeback
Reserved memory: created CMA memory pool at 0x8c000000, size 64 MiB
OF: fdt: Machine model: My Custom Board
cma: Reserved 64 MiB at 0x8c000000
Zone ranges:
  Normal   [mem 0x80000000-0x9fffffff]
Kernel command line: console=ttymxc0,115200 root=/dev/mmcblk0p2 rw rootwait

Early console in decompress_kernel

Uncompressing Linux... done, booting the kernel.

[    0.000000] Booting Linux on physical CPU 0x0
[    0.000000] Linux version 5.10.198 (anil@devhost) ...
[    0.100000] CPU0: i.MX6 Solo processor
[    0.150000] Built 1 zonelists, mobility grouping on.  Total pages: 130048
[    0.200000] Kernel command line: console=ttymxc0,115200 root=/dev/mmcblk0p2 rw rootwait
[    0.250000] PID hash table entries: 2048 (order: 1, 8192 bytes)

[    0.300000] console [ttymxc0] enabled
[    0.350000] mxs-dma 110000.dma-apbh: initialized
[    0.400000] imx_uart 21e8000.serial: detected UART at 0x21e8000
[    0.450000] imx_uart 21e8000.serial: ttyMXC0 at MMIO 0x21e8000 (irq = 59, base_baud = 5000000) is a IMX
[    0.500000] printk: console [ttymxc0] enabled

[    1.000000] mmc0: new high speed SDHC card at address aaaa
[    1.050000] mmcblk0: mmc0:aaaa SU16G 14.8 GiB
[    1.100000]  mmcblk0: p1 p2

[    1.500000] VFS: Cannot open root device "mmcblk0p2" or unknown-block(0,0): error -6
[    1.550000] Please append a correct "root=" boot option; here are the available partitions:
[    1.600000] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)

[    1.650000] CPU: 0 PID: 1 Comm: swapper Not tainted 5.10.198 #1
[    1.700000] ---[ end Kernel panic - not syncing: VFS: Unable to mount root fs ]---
```

---

## 🧠 Breakdown of What Happened:

| Stage            | What It Shows                                                                 |
| ---------------- | ----------------------------------------------------------------------------- |
| **SPL**          | Initialized DDR, loaded U-Boot from SD card                                   |
| **U-Boot**       | Loaded `zImage` and `myboard.dtb` into RAM                                    |
| **Kernel Start** | Detected SoC, RAM, serial console, printed DT info                            |
| **UART**         | Serial driver loaded successfully (`console=ttymxc0`)                         |
| **MMC/SD**       | SD card detected, partitions found                                            |
| **Kernel Panic** | No valid rootfs on `/dev/mmcblk0p2` → expected in minimal BSP (no rootfs yet) |

---

## ✅ This is a **successful minimal BSP bring-up**

Even though it ends in a kernel panic, the following confirms a good minimal BSP:

* Early printk works (console output visible)
* DTB is parsed (model string, memory, serial seen)
* Drivers for UART and MMC are loaded
* Kernel reached `start_kernel()` and tried mounting root

---

## 📌 How to Fix the Kernel Panic

To fully boot to shell:

* Add a valid root filesystem (e.g., BusyBox or Yocto rootfs)
* Pass correct `root=` argument (e.g., `root=/dev/mmcblk0p2`)
* Ensure filesystem exists on that partition (`ext4`, etc.)

---

## 🔧 Summary of What BSP Verified

| Component        | Verified in Logs |
| ---------------- | ---------------- |
| U-Boot load      | ✅                |
| Device Tree      | ✅                |
| UART console     | ✅                |
| Kernel entry     | ✅                |
| Memory detection | ✅                |
| Peripheral probe | ✅ (UART, MMC)    |
| Rootfs mount     | ❌ (Expected)     |

---

Let me know if you want:

* Real logs from a specific SoC (e.g., i.MX6, AM335x, STM32MP1)
* How to prepare a BusyBox-based rootfs to avoid panic
* How to trace kernel boot line-by-line (`start_kernel()`)

I can also generate a full **boot log + DTB + defconfig + image** package for practice.
