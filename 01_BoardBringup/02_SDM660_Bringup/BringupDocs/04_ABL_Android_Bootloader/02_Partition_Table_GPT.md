# GPT (GUID Partition Table) Parsing

## Overview

ABL reads the **GPT (GUID Partition Table)** from the storage device (UFS/eMMC) to discover all partitions. GPT is the modern replacement for MBR (Master Boot Record) and supports up to 128 partitions with unique GUIDs.

---

## GPT Structure on Disk

```
LBA 0 (512 bytes):
┌─────────────────────────────────────┐
│  Protective MBR                      │
│  (For backward compatibility)        │
│  Indicates GPT disk to legacy tools  │
└─────────────────────────────────────┘

LBA 1 (512 bytes):
┌─────────────────────────────────────┐
│  GPT Header                          │
│  ├── Signature: "EFI PART"          │
│  ├── Revision: 1.0                   │
│  ├── Header Size: 92 bytes          │
│  ├── CRC32 of header                │
│  ├── Current LBA: 1                 │
│  ├── Backup LBA: last LBA           │
│  ├── First usable LBA: 34           │
│  ├── Last usable LBA: N-34          │
│  ├── Disk GUID                       │
│  ├── Partition entry start LBA: 2   │
│  ├── Number of entries: 128         │
│  ├── Entry size: 128 bytes          │
│  └── CRC32 of partition entries     │
└─────────────────────────────────────┘

LBA 2-33 (16 KB):
┌─────────────────────────────────────┐
│  Partition Entry Array               │
│  128 entries × 128 bytes each        │
│                                     │
│  Entry format:                       │
│  ├── Partition Type GUID (16 bytes) │
│  ├── Unique Partition GUID (16 B)   │
│  ├── First LBA (8 bytes)            │
│  ├── Last LBA (8 bytes)             │
│  ├── Attributes (8 bytes)           │
│  └── Name (72 bytes, UTF-16)        │
└─────────────────────────────────────┘

LBA 34 onwards:
┌─────────────────────────────────────┐
│  Actual Partition Data               │
│  (xbl, tz, boot, system, etc.)      │
└─────────────────────────────────────┘

Last 33 LBAs:
┌─────────────────────────────────────┐
│  Backup Partition Entry Array        │
│  Backup GPT Header                   │
│  (Redundancy for recovery)           │
└─────────────────────────────────────┘
```

---

## How ABL Reads GPT

```
ABL Storage Init:
─────────────────
1. Initialize UFS/eMMC controller
2. Read LBA 0 — check for Protective MBR
3. Read LBA 1 — parse GPT header
   ├── Verify signature "EFI PART"
   ├── Verify CRC32 checksums
   └── Get partition entry count and start LBA
4. Read LBA 2-33 — parse all partition entries
5. Build partition table in memory:
   ├── Name → LBA offset mapping
   ├── "boot" → LBA 12345, size 131072 sectors
   ├── "system" → LBA 200000, size 6291456 sectors
   └── etc.
6. Partition table ready for use
```

---

## Partition Entry Details (SDM660 Example)

| Partition | Type GUID | First LBA | Size | Notes |
|-----------|-----------|-----------|------|-------|
| xbl | QC-specific | 34 | 512 KB | XBL bootloader |
| xbl_config | QC-specific | 1058 | 64 KB | XBL configuration |
| tz | QC-specific | 1186 | 1 MB | TrustZone firmware |
| rpm | QC-specific | 3234 | 256 KB | RPM firmware |
| boot | Android boot | 12345 | 64 MB | Kernel + ramdisk |
| recovery | Android boot | 143401 | 64 MB | Recovery image |
| system | Linux filesystem | 274457 | 3 GB | /system |
| vendor | Linux filesystem | 6568713 | 1 GB | /vendor |
| userdata | Linux filesystem | 8665769 | Remaining | /data |
| misc | QC-specific | 274201 | 1 MB | BCB flags |

---

## Partition Discovery by Name

ABL and the kernel locate partitions by **name**, not by position:

```c
/* In LK (aboot.c) */
/* Find boot partition by name */
int index = partition_get_index("boot");
unsigned long long offset = partition_get_offset(index);
unsigned long long size = partition_get_size(index);

/* Read boot image from partition */
mmc_read(offset, buf, size);
```

In Linux kernel, partitions are exposed via:
```
/dev/block/bootdevice/by-name/boot → /dev/block/sda11
/dev/block/bootdevice/by-name/system → /dev/block/sda15
```

---

## GPT Corruption Recovery

If the primary GPT is corrupted, the backup GPT (at end of disk) can be used:

```
ABL GPT Recovery:
─────────────────
1. Read primary GPT header (LBA 1)
2. Verify CRC32 → FAIL
3. Read backup GPT header (last LBA)
4. Verify backup CRC32 → PASS
5. Use backup GPT entries
6. Restore primary GPT from backup
```

---

## Related Documents

- [01_LK_Little_Kernel.md](01_LK_Little_Kernel.md) — ABL overview
- [03_Kernel_DTB_Loading.md](03_Kernel_DTB_Loading.md) — Loading kernel from boot partition
- [../00_SDM660_Architecture/06_Partition_Layout.md](../00_SDM660_Architecture/06_Partition_Layout.md) — Full partition layout
