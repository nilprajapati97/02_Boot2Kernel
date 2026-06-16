# Kernel & DTB Loading

## Overview

ABL loads three critical components from the `boot` partition: the **compressed kernel image**, the **ramdisk (initial root filesystem)**, and the **DTB (Device Tree Blob)**. These are packed together in an **Android boot image** format. This document explains the loading, verification, and handoff process.

---

## Boot Image Layout

```
boot partition (64 MB):
───────────────────────
┌──────────────────────────────────┐  Offset 0
│  Boot Image Header (v1 or v2)    │  Page-aligned (4096 bytes)
│  ├── magic: "ANDROID!"          │
│  ├── kernel_size                 │
│  ├── kernel_addr: 0x80080000    │
│  ├── ramdisk_size                │
│  ├── ramdisk_addr: 0x82200000   │
│  ├── tags_addr: 0x82000000      │
│  ├── page_size: 4096            │
│  └── cmdline: "console=..."     │
├──────────────────────────────────┤  Aligned to page_size
│  Kernel (Image.gz or Image.gz-dtb)│  ~15-25 MB
│  Compressed Linux kernel          │
│  (May have DTB appended)          │
├──────────────────────────────────┤  Aligned
│  Ramdisk (ramdisk.img)           │  ~5-10 MB
│  gzip'd CPIO archive             │
│  Contains: init, init.rc, etc.   │
├──────────────────────────────────┤  Aligned (if v2)
│  Second Stage (optional)          │  Usually empty
├──────────────────────────────────┤  Aligned (if v2)
│  DTB (if separate from kernel)    │  ~200 KB - 1 MB
└──────────────────────────────────┘
```

---

## Loading Process

```
ABL Kernel Loading:
───────────────────
1. Read boot image header (first page)
   ├── Verify magic == "ANDROID!"
   └── Extract sizes and addresses

2. Android Verified Boot (AVB) verification
   ├── Read vbmeta partition
   ├── Verify boot.img hash against vbmeta
   ├── If locked bootloader: hash MUST match
   └── If unlocked: warn but continue

3. Load kernel to memory
   ├── Read kernel_size bytes from boot partition
   ├── If Image.gz → decompress to kernel_addr
   ├── If Image.gz-dtb → separate kernel and DTB
   └── Kernel loaded at 0x80080000

4. Load ramdisk to memory
   ├── Read ramdisk_size bytes
   └── Load to ramdisk_addr (0x82200000)

5. Prepare Device Tree
   ├── Source: appended to kernel (-dtb) or DTB section
   ├── Match DTB using board ID / platform ID
   │   ├── Compare qcom,msm-id in DTB vs hardware fuses
   │   └── Select matching DTB for this specific board
   ├── Apply DTBO (overlay) if present
   │   ├── Read dtbo partition
   │   └── Overlay board-specific modifications
   └── DTB loaded at tags_addr (0x82000000)

6. Fixup Device Tree
   ├── Update /memory node with actual DDR size
   ├── Add /chosen/bootargs with kernel cmdline
   ├── Add /chosen/linux,initrd-start with ramdisk addr
   ├── Add /chosen/linux,initrd-end
   └── Set serial number, baseband info

7. Prepare for kernel entry
   ├── Flush all caches
   ├── Disable MMU
   ├── Disable interrupts
   └── Set CPU registers:
       x0 = DTB physical address (0x82000000)
       x1 = 0 (reserved)
       x2 = 0 (reserved)
       x3 = 0 (reserved)

8. Branch to kernel entry point
   └── PC = kernel_addr (0x80080000)
       → head.S in kernel starts executing
```

---

## DTB Matching

SDM660 boards may have multiple DTBs compiled for different board revisions. ABL selects the correct one:

```
DTB Selection:
──────────────
DTBs contain identification properties:
  qcom,msm-id = <0x127 0x0>;     /* SDM660, rev 0 */
  qcom,board-id = <0x20 0x0>;    /* MTP, subtype 0 */

ABL reads hardware:
  Platform ID from fuses → 0x127 (SDM660)
  Board ID from CDT/straps → 0x20 (MTP)

Match algorithm:
  For each DTB in Image.gz-dtb:
    if (dtb.msm_id == hw.platform_id &&
        dtb.board_id == hw.board_id)
      → Use this DTB
```

---

## DTBO (Device Tree Blob Overlay)

Android 9+ separates the device tree into:
- **Base DTB**: SoC-level (in boot.img with kernel)
- **DTBO**: Board-level overlays (in dtbo partition)

```
Base DTB (sdm660.dtsi compiled)
    │
    ▼ apply overlay
DTBO (board-specific.dtso compiled)
    │
    ▼
Final DTB (merged, passed to kernel)

Example overlay:
────────────────
/* Enable I2C bus 3 for BMI160 sensor */
&i2c_3 {
    status = "okay";
    bmi160@68 {
        compatible = "bosch,bmi160";
        reg = <0x68>;
    };
};
```

---

## Android Verified Boot (AVB)

```
AVB Verification Flow:
──────────────────────
1. ABL reads vbmeta partition
   └── Contains: hash descriptors for boot, system, vendor

2. For boot.img:
   ├── Compute SHA-256 hash of boot.img
   ├── Compare with hash in vbmeta
   └── Verify vbmeta RSA signature with OEM key

3. Verification result:
   ├── GREEN: All verified, locked bootloader
   ├── YELLOW: User-signed vbmeta (custom key)
   ├── ORANGE: Unlocked bootloader (dev mode)
   └── RED: Verification failed → refuse to boot

4. Set androidboot.verifiedbootstate=<color>
```

---

## Memory Map After Loading

```
Physical Address Map (after ABL loading):
─────────────────────────────────────────
0x80000000 ┌────────────────────────────┐
           │  (gap)                      │
0x80080000 ├────────────────────────────┤
           │  Linux Kernel (decompressed)│  ~20 MB
           │  Image loaded here          │
0x81480000 ├────────────────────────────┤
           │  (gap)                      │
0x82000000 ├────────────────────────────┤
           │  DTB (Device Tree Blob)     │  ~1 MB
0x82100000 ├────────────────────────────┤
           │  (gap)                      │
0x82200000 ├────────────────────────────┤
           │  Ramdisk (init + init.rc)   │  ~10 MB
0x82C00000 ├────────────────────────────┤
           │  Available memory           │
           │  (managed by Linux MM)      │
           └────────────────────────────┘
```

---

## Related Documents

- [01_LK_Little_Kernel.md](01_LK_Little_Kernel.md) — ABL overview
- [04_Boot_Mode_Selection.md](04_Boot_Mode_Selection.md) — Normal vs recovery vs fastboot
- [../05_Linux_Kernel_Boot/01_Early_Assembly_Boot.md](../05_Linux_Kernel_Boot/01_Early_Assembly_Boot.md) — Kernel entry (head.S)
- [../05_Linux_Kernel_Boot/02_Device_Tree_Processing.md](../05_Linux_Kernel_Boot/02_Device_Tree_Processing.md) — How kernel parses DTB
