# Partition Layout — SDM660 Storage

## Overview

The SDM660 platform uses **UFS (Universal Flash Storage)** or **eMMC** as its primary storage device. The storage is divided into multiple **partitions** using a **GPT (GUID Partition Table)**. Each partition holds a specific firmware image, filesystem, or data region critical to the boot chain.

---

## Partition Table Overview

```
┌────────────────────────────────────────────────────────────────┐
│                    Storage Device (UFS/eMMC)                    │
│                                                                │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐      │
│  │ GPT  │ │ xbl  │ │xbl_  │ │ tz   │ │ rpm  │ │hyp   │      │
│  │Header│ │      │ │config│ │      │ │      │ │      │      │
│  │      │ │256KB │ │64KB  │ │1MB   │ │256KB │ │512KB │      │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘      │
│                                                                │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐      │
│  │pmic  │ │keyma │ │cmnlib│ │cmnlib│ │devcfg│ │aboot │      │
│  │      │ │ster  │ │      │ │64    │ │      │ │(ABL) │      │
│  │64KB  │ │256KB │ │256KB │ │256KB │ │64KB  │ │2MB   │      │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘      │
│                                                                │
│  ┌──────┐ ┌──────┐ ┌──────────────┐ ┌──────────────┐         │
│  │ boot │ │recov │ │   system     │ │   vendor     │         │
│  │      │ │ery   │ │              │ │              │         │
│  │ 64MB │ │ 64MB │ │   3-4 GB     │ │   1-2 GB     │         │
│  └──────┘ └──────┘ └──────────────┘ └──────────────┘         │
│                                                                │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────────────────────┐         │
│  │cache │ │misc  │ │persi │ │     userdata          │         │
│  │      │ │      │ │st    │ │     (remaining)       │         │
│  │256MB │ │1MB   │ │32MB  │ │                       │         │
│  └──────┘ └──────┘ └──────┘ └──────────────────────┘         │
└────────────────────────────────────────────────────────────────┘
```

---

## Complete Partition Table

### Boot Chain Partitions

| Partition | Size | Image | Purpose | Boot Phase |
|-----------|------|-------|---------|------------|
| `xbl` | 256 KB | XBL firmware | eXtensible Boot Loader — first code from flash | PBL loads this |
| `xbl_config` | 64 KB | XBL config | XBL configuration data | XBL reads this |
| `tz` | 1 MB | TrustZone | Secure world firmware (QSEE) | XBL loads this |
| `rpm` | 256 KB | RPM firmware | Resource Power Manager firmware | XBL loads this |
| `hyp` | 512 KB | Hypervisor | Hypervisor firmware (EL2) | XBL loads this |
| `pmic` | 64 KB | PMIC config | PMIC configuration firmware | XBL reads this |
| `keymaster` | 256 KB | Keymaster TA | Hardware-backed keystore | TZ loads this |
| `cmnlib` | 256 KB | Common lib | Shared library for trusted apps (32-bit) | TZ loads this |
| `cmnlib64` | 256 KB | Common lib 64 | Shared library for trusted apps (64-bit) | TZ loads this |
| `devcfg` | 64 KB | Device config | Device configuration blob | XBL reads this |
| `aboot` / `abl` | 2 MB | LK / ABL | Android Boot Loader (Little Kernel) | XBL loads this |

### Android Partitions

| Partition | Size | Filesystem | Purpose |
|-----------|------|------------|---------|
| `boot` | 64 MB | Boot image | Kernel (Image.gz) + ramdisk + DTB |
| `recovery` | 64 MB | Boot image | Recovery kernel + ramdisk |
| `system` | 3-4 GB | ext4 / erofs | Android framework (`/system`) |
| `vendor` | 1-2 GB | ext4 | Vendor HALs, firmware (`/vendor`) |
| `product` | 512 MB | ext4 | Product-specific overlays |
| `cache` | 256 MB | ext4 | OTA cache, temp storage |
| `userdata` | Remaining | f2fs / ext4 | User data, apps (`/data`) |

### Metadata & Control Partitions

| Partition | Size | Purpose |
|-----------|------|---------|
| `misc` | 1 MB | Bootloader Control Block (BCB) — reboot-to-recovery flags |
| `persist` | 32 MB | Persistent data across factory resets (WiFi MAC, calibration) |
| `metadata` | 2 MB | FDE/FBE encryption metadata |
| `frp` | 512 KB | Factory Reset Protection |
| `devinfo` | 1 MB | Device info (unlock state, tamper fuses) |
| `logdump` | 64 MB | Crash/ramdump logs |
| `fsc` | 128 KB | Modem file system cache |
| `modemst1` | 2 MB | Modem NV (non-volatile) items |
| `modemst2` | 2 MB | Modem NV backup |
| `dsp` | 64 MB | ADSP/CDSP/SLPI firmware images |

### A/B Partition Scheme (if enabled)

On devices with **A/B (seamless) updates**, boot chain partitions are duplicated:

```
xbl_a / xbl_b
tz_a / tz_b
rpm_a / rpm_b
boot_a / boot_b
system_a / system_b
vendor_a / vendor_b
```

---

## Boot Image Format (`boot` partition)

The `boot` partition contains an **Android boot image** with this structure:

```
┌───────────────────────────┐
│  Boot Image Header (v1/v2) │  2048 bytes
│  - kernel_size              │
│  - ramdisk_size             │
│  - cmdline                  │
│  - page_size                │
├───────────────────────────┤
│  Kernel (Image.gz)         │  ~15-25 MB
│  Compressed kernel binary   │
├───────────────────────────┤
│  Ramdisk (ramdisk.img)     │  ~5-10 MB
│  CPIO archive (gzipped)    │
│  Contains init, init.rc    │
├───────────────────────────┤
│  DTB (device tree blob)     │  ~200 KB - 1 MB
│  Appended or separate       │
└───────────────────────────┘
```

### Boot Image Header Fields

```c
struct boot_img_hdr {
    uint8_t  magic[8];           /* "ANDROID!" */
    uint32_t kernel_size;
    uint32_t kernel_addr;        /* Physical load address */
    uint32_t ramdisk_size;
    uint32_t ramdisk_addr;
    uint32_t second_size;        /* Second-stage bootloader */
    uint32_t second_addr;
    uint32_t tags_addr;          /* DTB load address */
    uint32_t page_size;          /* Flash page size (4096) */
    uint32_t header_version;
    uint32_t os_version;
    char     name[16];           /* Product name */
    char     cmdline[512];       /* Kernel command line */
    uint32_t id[8];              /* SHA hash */
    char     extra_cmdline[1024];
};
```

---

## GPT (GUID Partition Table) Structure

```
LBA 0:  Protective MBR
LBA 1:  Primary GPT Header
LBA 2-33: Primary GPT Entries (128 entries × 128 bytes)
...
LBA N-33 to N-2: Backup GPT Entries
LBA N-1: Backup GPT Header
```

### Viewing Partitions on Device

```bash
# List all partitions
adb shell ls -la /dev/block/bootdevice/by-name/

# Output example:
# boot -> /dev/block/sda11
# system -> /dev/block/sda15
# vendor -> /dev/block/sda16
# userdata -> /dev/block/sda20

# View partition sizes
adb shell cat /proc/partitions

# Read GPT with sgdisk
adb shell sgdisk --print /dev/block/sda
```

---

## Partition Usage During Boot

```
PBL  ──reads──▶  xbl partition
                    │
XBL  ──reads──▶  tz, rpm, hyp, pmic, devcfg partitions
                    │
ABL  ──reads──▶  boot partition (kernel + ramdisk + DTB)
                    │
                 Also reads: misc (BCB), devinfo (unlock state)
                    │
Kernel ──mounts──▶ system, vendor partitions
                    │
Android ──mounts──▶ userdata, cache, persist partitions
```

---

## Flashing Partitions

```bash
# Enter fastboot mode
adb reboot bootloader

# Flash individual partitions
fastboot flash boot boot.img
fastboot flash system system.img
fastboot flash vendor vendor.img

# Erase userdata (factory reset)
fastboot erase userdata
fastboot erase cache

# Flash boot chain (requires unlocked bootloader)
fastboot flash xbl xbl.elf
fastboot flash tz tz.mbn
fastboot flash rpm rpm.mbn
fastboot flash aboot emmc_appsboot.mbn

# Reboot
fastboot reboot
```

---

## Related Documents

- [01_SoC_Block_Diagram.md](01_SoC_Block_Diagram.md) — SoC architecture overview
- [../01_Power_On_Reset/02_PBL_Primary_Bootloader.md](../01_Power_On_Reset/02_PBL_Primary_Bootloader.md) — PBL reads xbl partition
- [../04_ABL_Android_Bootloader/02_Partition_Table_GPT.md](../04_ABL_Android_Bootloader/02_Partition_Table_GPT.md) — GPT parsing details
