# ABL — Android Boot Loader (Little Kernel)

## Overview

**ABL (Android Boot Loader)**, based on **LK (Little Kernel)**, is the final bootloader stage before the Linux kernel. It provides the **fastboot** interface for flashing, reads the GPT partition table, loads the kernel and device tree from the `boot` partition, and jumps to the kernel entry point.

---

## LK Architecture

```
┌────────────────────────────────────────────────────────┐
│                 ABL / LK Architecture                   │
│                                                        │
│  ┌──────────────────────────────────────────────────┐  │
│  │                  LK Core                          │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐         │  │
│  │  │ Platform │ │ Target   │ │ App      │         │  │
│  │  │ (SoC)    │ │ (Board)  │ │ (aboot)  │         │  │
│  │  │          │ │          │ │          │         │  │
│  │  │ Clock    │ │ GPIO     │ │ Fastboot │         │  │
│  │  │ UART     │ │ Keys     │ │ Boot     │         │  │
│  │  │ USB      │ │ Display  │ │ Recovery │         │  │
│  │  │ Storage  │ │ Panel    │ │ Kernel   │         │  │
│  │  │          │ │          │ │ Loading  │         │  │
│  │  └──────────┘ └──────────┘ └──────────┘         │  │
│  │                                                  │  │
│  │  ┌──────────────────────────────────────┐       │  │
│  │  │         LK Kernel                     │       │  │
│  │  │  Threading, Timer, Heap, Printf       │       │  │
│  │  └──────────────────────────────────────┘       │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

---

## ABL Boot Flow

```
XBL jumps to ABL entry point
    │
    ▼
┌──────────────────────────────────────────────────────────┐
│                    ABL Execution Flow                      │
│                                                          │
│  1. Early Init                                           │
│     ├── Initialize LK threading/timer                    │
│     ├── Initialize debug UART (serial output)            │
│     └── Initialize target hardware (GPIO, buttons)       │
│                                                          │
│  2. Storage Init                                         │
│     ├── Initialize UFS/eMMC controller                   │
│     └── Read GPT (GUID Partition Table)                  │
│         └── Discover all partitions                      │
│                                                          │
│  3. Boot Mode Selection                                  │
│     ├── Check key combos (Vol Down = fastboot)           │
│     ├── Check BCB in misc partition                      │
│     │   ├── "boot-recovery" → boot to recovery          │
│     │   └── empty → normal boot                          │
│     └── Check reboot reason register                     │
│                                                          │
│  4. Display Init (if splash screen)                      │
│     ├── Initialize DSI PHY                               │
│     ├── Initialize display panel                         │
│     └── Show OEM logo / splash screen                    │
│                                                          │
│  5a. If Fastboot Mode:                                   │
│      ├── Initialize USB (DWC3 in device mode)            │
│      ├── Start fastboot protocol handler                 │
│      └── Wait for USB commands from host                 │
│                                                          │
│  5b. If Normal Boot:                                     │
│      ├── Read boot partition                             │
│      │   ├── Parse boot image header                     │
│      │   ├── Extract kernel (Image.gz)                   │
│      │   ├── Extract ramdisk                             │
│      │   └── Extract DTB                                 │
│      ├── Verify boot image (AVB / dm-verity)             │
│      ├── Prepare kernel command line                     │
│      │   ├── androidboot.hardware=qcom                   │
│      │   ├── androidboot.serialno=<serial>               │
│      │   ├── console=ttyMSM0,115200n8                    │
│      │   └── Other board-specific params                 │
│      ├── Load kernel to designated DDR address           │
│      ├── Load DTB/ramdisk to designated addresses        │
│      └── Jump to kernel entry point                      │
│           └── x0 = DTB physical address (ARM64)          │
└──────────────────────────────────────────────────────────┘
```

---

## Fastboot Interface

Fastboot provides a USB-based protocol for interacting with the bootloader:

```bash
# Common fastboot commands
fastboot devices                    # List connected devices
fastboot getvar all                 # Show device variables
fastboot flash boot boot.img       # Flash boot partition
fastboot flash system system.img   # Flash system partition
fastboot erase userdata             # Erase user data
fastboot boot boot.img             # Boot without flashing
fastboot reboot                    # Reboot device
fastboot oem unlock                # Unlock bootloader (OEM-specific)
fastboot oem device-info            # Show device lock/unlock status
```

### Fastboot Variables

```
fastboot getvar all
─────────────────────
version: 0.5
variant: SDM660 MTP
max-download-size: 536870912
serialno: ABC123DEF456
product: sdm660
secure: yes
unlocked: no
off-mode-charge: 1
charger-screen-enabled: 1
battery-voltage: 4100
battery-soc-ok: yes
```

---

## Kernel Command Line Construction

ABL constructs the kernel command line dynamically:

```
Final cmdline (assembled by ABL):
──────────────────────────────────
console=ttyMSM0,115200n8
androidboot.console=ttyMSM0
androidboot.hardware=qcom
androidboot.memcg=1
androidboot.bootdevice=1da4000.ufshc
androidboot.serialno=ABC123DEF456
androidboot.baseband=sdm
androidboot.verifiedbootstate=orange
androidboot.keymaster=1
androidboot.dtbo_idx=0
lpm_levels.sleep_disabled=1          (disabled during bring-up)
earlycon=msm_serial_dm,0x78af000    (early UART console)
```

---

## LK Source Structure

```
bootable/bootloader/lk/
├── app/
│   └── aboot/
│       └── aboot.c            # *** Main boot application ***
│           ├── aboot_init()    # Entry point
│           ├── boot_linux()    # Loads and boots kernel
│           └── fastboot_init() # Fastboot handler
├── platform/
│   └── sdm660/
│       ├── platform.c         # Platform init (clocks, PMIC)
│       ├── gpio.c             # GPIO configuration
│       └── acpuclock.c        # CPU clock setup
├── target/
│   └── sdm660/
│       ├── target.c           # Target/board-specific init
│       ├── init.c             # Board init (keys, display)
│       └── keypad.c           # Key detection (Vol Up/Down)
├── dev/
│   ├── fbcon/                 # Framebuffer console
│   ├── panel/                 # Display panel drivers
│   └── pmic/                  # PMIC access
├── lib/
│   ├── bio/                   # Block I/O
│   ├── partition/             # GPT partition parsing
│   └── libfdt/               # Device tree manipulation
└── kernel/                    # LK micro-kernel
    ├── thread.c               # Threading
    ├── timer.c                # Timer
    └── main.c                 # LK entry point → kmain()
```

---

## Kernel Loading Details

```
ABL reads boot partition:
─────────────────────────
1. Seek to boot partition start
2. Read boot image header (first page)
3. Parse: kernel_size, ramdisk_size, cmdline, page_size

4. Load kernel to kernel_addr (0x80080000 typical)
   ├── Decompress if gzipped
   └── Size: 15-25 MB

5. Load ramdisk to ramdisk_addr (0x82200000 typical)
   └── Size: 5-10 MB

6. Load/prepare DTB
   ├── If appended to kernel: extract from Image.gz-dtb
   ├── If separate DTBO: load from dtbo partition, apply overlay
   └── DTB address: passed via x0 register on ARM64

7. Disable caches and MMU (clean state for kernel)
8. Set CPU registers:
   ├── x0 = DTB physical address
   ├── x1 = 0 (reserved)
   ├── x2 = 0 (reserved)
   └── x3 = 0 (reserved)
9. Branch to kernel entry (head.S)
```

---

## Related Documents

- [02_Partition_Table_GPT.md](02_Partition_Table_GPT.md) — How ABL reads GPT
- [03_Kernel_DTB_Loading.md](03_Kernel_DTB_Loading.md) — Kernel + DTB loading details
- [04_Boot_Mode_Selection.md](04_Boot_Mode_Selection.md) — Fastboot/recovery/normal selection
- [../02_XBL_Secondary_Bootloader/01_XBL_Overview.md](../02_XBL_Secondary_Bootloader/01_XBL_Overview.md) — XBL loads ABL
- [../05_Linux_Kernel_Boot/01_Early_Assembly_Boot.md](../05_Linux_Kernel_Boot/01_Early_Assembly_Boot.md) — What happens after kernel entry
