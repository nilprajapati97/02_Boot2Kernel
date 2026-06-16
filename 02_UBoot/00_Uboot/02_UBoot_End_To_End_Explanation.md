Here is a **deep, end-to-end explanation of Linux booting from scratch** on an embedded board, including:

* Boot stages (ROM → SPL → U-Boot → Kernel)
* What initializes at each stage
* Real boot log lines with explanations
* Developer-level insights into **RAM init, peripheral bring-up, device tree handoff**, etc.

---

## 🧭 **Linux Boot Process: Complete Flow from Scratch**

```plaintext
[1] Power-On Reset
     ↓
[2] BootROM (Internal ROM code in SoC)
     ↓
[3] SPL (Secondary Program Loader / MLO)
     ↓
[4] U-Boot Proper
     ↓
[5] Linux Kernel (zImage/uImage)
     ↓
[6] Root Filesystem (init/systemd)
```

---

## 🔧 \[1] **BootROM (SoC Internal ROM Code)**
==========================================================================================================
### ✅ Purpose:

* Executes from internal ROM after reset.
* Configures minimal hardware (e.g. PLL, watchdog, boot pins).
* Loads SPL from boot device (eMMC, NAND, SD, USB).

### 🔍 Logs:

BootROM typically doesn't output anything, but if UART ROM debug is enabled:

```plaintext
BootROM: Boot device detected = MMC1
BootROM: Loading SPL
```

### 💡 Developer Tasks:

* Ensure boot pins or fuses are set to correct boot source.
* Format boot media (e.g., SD card) correctly (SPL at sector 0).

---

## 🪜 \[2] **SPL (Secondary Program Loader)**
=======================================================================================================
### ✅ Purpose:

* Minimal bootloader (tiny U-Boot).
* Initializes **DRAM**, UART, and PMIC.
* Loads full U-Boot into RAM and jumps to it.

### 🔧 Key Initializations:

* DDR controller (mandatory before loading large binaries)
* UART (for logs)
* PMIC or regulators (if used)

### 🔍 Logs:

```plaintext
U-Boot SPL 2021.10 (Jul 25 2025 - 10:10:00)
Trying to boot from MMC1
SPL: Initializing DRAM... done
```

### 💡 Developer Tasks:

* Write board-specific DDR init code (`spl.c`, `ddr.c`)
* Validate DRAM init via logs or `printf()`
* Link SPL with minimal features using `Kconfig` and `defconfig`

---

## 🚀 \[3] **U-Boot Proper**
==========================================================================================================

### ✅ Purpose:

* Full bootloader with rich features.
* Loads and boots the Linux kernel (zImage/uImage), DTB, and initrd.
* Sets up environment, peripherals, etc.

### 🔧 Initializations in order:

| Function          | Purpose                          |
| ----------------- | -------------------------------- |
| `board_init_f()`  | Initializes CPU, console, clocks |
| `init_sequence_f` | Runs DRAM, timers, UART, etc.    |
| `dram_init()`     | DRAM size detection              |
| `board_init_r()`  | Enables peripherals              |
| `env_init()`      | Load env from flash/emmc         |
| `mmc_init()`      | Detect SD/eMMC                   |
| `net_init()`      | Bring up Ethernet                |

### 🔍 Logs:

```plaintext
U-Boot 2021.10 (Jul 25 2025 - 10:10:05)
CPU:   i.MX6 Quad @ 1.2 GHz
DRAM:  1 GiB
MMC:   FSL_SDHC: 0
Net:   FEC [PRIME]
Hit any key to stop autoboot:  3
```

---

## 🏁 \[4] **Kernel Load and Execution**
==========================================================================================================
### ✅ U-Boot Command:

```bash
bootz 0x82000000 - 0x83000000
```

| Addr         | What is Loaded       |
| ------------ | -------------------- |
| `0x82000000` | zImage (kernel)      |
| `-`          | initramfs (optional) |
| `0x83000000` | DTB (device tree)    |

### 🔍 Logs:

```plaintext
## Flattened Device Tree blob at 83000000
Booting using the fdt blob at 0x83000000
Using Device Tree in place at 83000000, end 0x83010000
```

U-Boot hands off to kernel entry point using `bootz/bootm`.

---

## 🧩 \[5] **Kernel Decompression and Boot**
==========================================================================================================

### ✅ What Happens:

* `zImage` has decompressor stub
* Decompresses real kernel into RAM
* Kernel initializes subsystems (MMU, scheduler, init, etc.)

### 🔍 Logs:

```plaintext
Uncompressing Linux... done, booting the kernel.
Booting Linux on physical CPU 0x0
Linux version 5.10.XX (gcc version ...)
```

### Kernel Init Sequence (simplified):

1. `start_kernel()` (in `init/main.c`)
2. `setup_arch()` → reads device tree
3. `initcalls` → drivers and subsystems
4. Mount rootfs (initramfs/NFS/MMC)
5. Execute `/init` or `/sbin/init`

---

## 🏗️ \[6] **Root Filesystem and Init**
==========================================================================================================

### ✅ What Happens:

* Mounts root filesystem from MMC/eMMC/NFS/initramfs
* Runs `init` or `systemd`

### 🔍 Logs:

```plaintext
EXT4-fs (mmcblk0p2): mounted filesystem
Run /sbin/init as init process
systemd[1]: Starting basic target...
```

---

## 📌 Boot Flow Summary
==========================================================================================================
| Stage   | Who Runs          | Output Seen / Log | What You Should Debug   |
| ------- | ----------------- | ----------------- | ----------------------- |
| BootROM | SoC internal      | Usually nothing   | Boot mode, SD image     |
| SPL     | U-Boot SPL        | SPL version, DRAM | DRAM init, UART         |
| U-Boot  | U-Boot proper     | Board info        | MMC, env, DTB           |
| Kernel  | Kernel decompress | “Uncompressing…”  | Kernel crash, DT issues |
| Init    | `init`, systemd   | Login prompt      | Rootfs, userland        |

---

## 🧠 Tips for Debugging and Interview

### ✅ If no kernel logs:

* Ensure UART clocks match between U-Boot and kernel
* Check kernel’s console cmdline (`console=ttyS0,115200`)

### ✅ If U-Boot hangs:

* Add debug `printf()` in `board_init_f()`
* Confirm DRAM initialized (use `md` to test)

### ✅ If Linux hangs:

* Use earlyprintk
* Enable `CONFIG_DEBUG_LL`
* Trace via JTAG if needed

---

Would you like:
==========================================================================================================
* A **PDF visual boot map**?
* Or detailed **cmdline and DTB analysis**?
* Or a full example project (e.g., SD boot, U-Boot config, defconfig)?
