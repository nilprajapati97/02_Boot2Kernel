# Boot Mode Selection

## Overview

ABL determines which boot mode to enter based on **key combinations**, the **BCB (Bootloader Control Block)** in the `misc` partition, and **reboot reason registers**. The three primary modes are: **Normal Boot**, **Recovery Mode**, and **Fastboot Mode**.

---

## Boot Mode Decision Flow

```
ABL starts
    │
    ▼
┌──────────────────────────────────────────┐
│  Check hardware keys                      │
│  ├── Volume Down held? → FASTBOOT        │
│  ├── Volume Up held?   → RECOVERY        │
│  └── No keys → continue                  │
└──────────────────┬───────────────────────┘
                   │
    ┌──────────────▼──────────────────────┐
    │  Check reboot reason register       │
    │  (PMIC PON register / IMEM flag)    │
    │  ├── "bootloader" → FASTBOOT        │
    │  ├── "recovery"   → RECOVERY        │
    │  ├── "edl"        → EMERGENCY DL    │
    │  └── Other/none   → continue        │
    └──────────────┬─────────────────────┘
                   │
    ┌──────────────▼──────────────────────┐
    │  Read BCB from misc partition       │
    │  ├── command = "boot-recovery"      │
    │  │   → RECOVERY                     │
    │  ├── command = "update-radio"       │
    │  │   → RECOVERY (radio update)      │
    │  └── command = empty                │
    │      → NORMAL BOOT                  │
    └──────────────┬─────────────────────┘
                   │
                   ▼
            NORMAL BOOT
```

---

## Boot Modes

### Normal Boot

```
Load kernel + ramdisk from boot partition
    → Boot Linux kernel
    → Android starts normally
```

### Fastboot Mode

```
Initialize USB in device mode (DWC3)
    → Start fastboot protocol handler
    → Wait for USB commands from host PC
    → Supports: flash, erase, boot, reboot, oem commands
    → Exit via: fastboot reboot
```

### Recovery Mode

```
Load kernel + ramdisk from recovery partition
    → Boot recovery Linux kernel
    → Recovery UI (factory reset, sideload OTA, wipe cache)
    → Used for: OTA updates, factory reset, ADB sideload
```

### Emergency Download (EDL) Mode

```
Not handled by ABL — PBL-level
    → Device enters Qualcomm 9008 USB mode
    → Used for: complete reflash via QFIL/QPST
    → Triggered by: edl reboot reason, or hardware straps
```

---

## BCB (Bootloader Control Block)

The BCB is stored in the `misc` partition and is the primary mechanism for Android to communicate boot instructions to the bootloader.

```c
/* BCB structure in misc partition */
struct bootloader_message {
    char command[32];       /* "boot-recovery", "update-radio", etc. */
    char status[32];        /* Status for update result */
    char recovery[768];     /* Recovery command line */
    char stage[32];         /* Multi-stage update progress */
    char reserved[1184];    /* Reserved space */
};
```

### How BCB is Used

```
OTA Update Flow:
────────────────
1. Android OTA app downloads update package
2. Writes to BCB:
   command = "boot-recovery"
   recovery = "--update_package=/cache/ota.zip"
3. Reboots

4. ABL reads misc partition → sees "boot-recovery"
5. ABL boots recovery partition

6. Recovery reads BCB → sees "--update_package=..."
7. Recovery applies OTA update
8. Recovery clears BCB command
9. Recovery reboots → ABL reads empty BCB → Normal boot
```

### Factory Reset via BCB

```
User selects "Factory Reset" in Settings
    │
    ▼
Android writes to BCB:
  command = "boot-recovery"
  recovery = "--wipe_data"
    │
    ▼
Reboot → ABL boots recovery → Recovery wipes /data → Normal boot
```

---

## Reboot Reason

When Android requests a specific reboot mode:

```bash
# From Android
adb reboot                 # Normal reboot
adb reboot bootloader      # Reboot to fastboot
adb reboot recovery        # Reboot to recovery
adb reboot edl             # Reboot to EDL (9008)
```

The reboot command writes a reason to either:
- **PMIC PON SOFT_RESET register** (survives power cycle)
- **IMEM scratch register** (survives warm reset only)

```
Reboot flow:
1. reboot("bootloader") called
2. Kernel writes "bootloader" to IMEM scratch register (0x0014_5B6C)
3. Kernel triggers PSCI SYSTEM_RESET
4. SoC resets → PBL → XBL → ABL
5. ABL reads IMEM scratch → "bootloader" → enters fastboot
6. ABL clears the scratch register
```

---

## Key Detection

ABL reads GPIO-connected keys during early init:

```
Hardware Key Mapping (typical SDM660 MTP):
──────────────────────────────────────────
Volume Up:   PM660 GPIO 2 (PMIC GPIO)
Volume Down: PM660 GPIO 5 (PMIC GPIO)
Power:       PM660 KPDPWR (PON module)

Detection:
  ABL reads PMIC GPIO status via SPMI
  If key is pressed (GPIO low):
    Vol Down → fastboot_mode = true
    Vol Up   → recovery_mode = true
    Vol Up + Vol Down → crash dump mode (some boards)
```

---

## ABL Debug: Boot Mode Logging

```
ABL serial output:
──────────────────
[ABL] Checking boot mode...
[ABL] Key status: Vol_Up=0, Vol_Down=1
[ABL] Reboot reason: none
[ABL] BCB command: empty
[ABL] Boot mode: FASTBOOT
[ABL] Starting fastboot...

Or:
[ABL] Key status: Vol_Up=0, Vol_Down=0
[ABL] Reboot reason: none
[ABL] BCB command: empty
[ABL] Boot mode: NORMAL
[ABL] Loading boot image...
```

---

## Related Documents

- [01_LK_Little_Kernel.md](01_LK_Little_Kernel.md) — ABL overview
- [03_Kernel_DTB_Loading.md](03_Kernel_DTB_Loading.md) — Normal boot kernel loading
- [02_Partition_Table_GPT.md](02_Partition_Table_GPT.md) — misc partition location
