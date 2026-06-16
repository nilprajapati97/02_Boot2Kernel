Setting up boot media (eMMC, SD, NOR/NAND flash) in an **Embedded Linux Board** involves configuring the **bootloader**, **partition layout**, and **flashing the kernel, DTB, and rootfs** properly.

---

# ⚙️ How to Set Up Boot Media in Embedded Linux

We’ll cover:

1. ✅ Understanding the boot sequence
2. 🧱 Media-specific setup (eMMC, SD card, NOR, NAND)
3. 🔧 Partitioning & flashing
4. 🧪 Bootloader configuration
5. 🛠️ Troubleshooting tips

---

## 🔁 1. Linux Boot Sequence (Simplified)

```plaintext
[ Boot ROM (internal) ]
        ↓
[ SPL / MLO (1st stage bootloader) ]
        ↓
[ U-Boot (2nd stage bootloader) ]
        ↓
[ Linux Kernel + DTB ]
        ↓
[ Root filesystem (eMMC, SD, NFS, UBIFS, etc.) ]
```

Each boot media must contain the correct components in the correct offsets or partitions.

---

## 🧱 2. Boot Media Setup

---

### 📀 A. **eMMC Boot Setup**

**Used in:** Production-grade boards (e.g., BeagleBone, i.MX, TI AM335x)

#### 🔹 Steps:

1. **Partition eMMC** using `fdisk` or `parted`
2. **Flash components**:

   ```bash
   dd if=MLO of=/dev/mmcblk1 bs=512 seek=2
   dd if=u-boot.img of=/dev/mmcblk1 bs=512 seek=69
   ```

   * Use proper `seek` values per SoC's boot ROM spec.
3. Flash `zImage` or `Image` to `/boot` partition
4. Flash `rootfs` (ext4) to second partition

#### 🔹 Mount points:

```plaintext
/dev/mmcblk1p1 → boot
/dev/mmcblk1p2 → rootfs
```

#### 📌 Notes:

* Many SoCs require **MLO at a specific offset**.
* eMMC has two boot partitions: `boot0`, `boot1`, and a general-purpose `user`.

---

### 💾 B. **SD Card Boot Setup**

**Used in:** Development boards (Raspberry Pi, BeagleBone, etc.)

#### 🔹 Format:

```bash
fdisk /dev/sdX
# Partition 1: FAT32 → for bootloader, kernel, DTB
# Partition 2: ext4 → rootfs
```

#### 🔹 Flash:

```bash
dd if=MLO of=/dev/sdX bs=512 seek=1
dd if=u-boot.img of=/dev/sdX bs=512 seek=69
cp zImage *.dtb /mnt/boot/
cp -a rootfs/* /mnt/rootfs/
```

#### 🔹 Boot configuration:

* U-Boot loads `boot.scr` or uses `boot.cmd`
* You may use `extlinux.conf` for modern distros (boot with extlinux)

---

### 🔥 C. **NOR Flash Boot**

**Used in:** Industrial-grade boards, usually small NOR for bootloader.

#### 🔹 Characteristics:

* Fast read, small capacity (1MB–16MB)
* Usually memory-mapped

#### 🔹 Setup:

* Use `flash_erase` and `nandwrite` or `mtd_debug`:

```bash
flash_erase /dev/mtd0 0 0
nandwrite -p /dev/mtd0 u-boot.bin
```

* NOR stores: **U-Boot**, env variables
* Kernel + rootfs loaded from NAND or eMMC/SD

#### 🔹 Flash layout (via DTS or U-Boot):

```dts
partitions {
    compatible = "fixed-partitions";
    partition@0 {
        label = "u-boot";
        reg = <0x0 0x40000>;
    };
    ...
};
```

---

### 📦 D. **NAND Flash Boot**

**Used in:** Cost-sensitive embedded devices

#### 🔹 Characteristics:

* Large capacity, block-based
* Needs ECC handling

#### 🔹 Setup:

1. **Partition** NAND via DTS or U-Boot
2. Flash with `ubiformat` / `ubiattach` for UBI support:

```bash
flash_erase /dev/mtd0 0 0
nandwrite -p /dev/mtd0 u-boot.bin

# For rootfs
ubiformat /dev/mtd3 -f rootfs.ubi
ubiattach -m 3
mount -t ubifs ubi0:rootfs /mnt
```

3. U-Boot must support UBI/UBIFS if rootfs is in NAND.

---

## 📁 3. Typical Partition Layout Examples

### SD/eMMC (MBR/GPT):

| Partition | Type  | Purpose         |
| --------- | ----- | --------------- |
| p1        | FAT32 | Bootloader, DTB |
| p2        | ext4  | Root filesystem |

---

### NAND/NOR (MTD-based):

| Partition | MTD Name   | Purpose    |
| --------- | ---------- | ---------- |
| mtd0      | bootloader | U-Boot     |
| mtd1      | env        | U-Boot env |
| mtd2      | kernel     | zImage/DTB |
| mtd3      | rootfs     | UBI volume |

---

## 🧪 4. U-Boot Environment Configuration

```bash
setenv bootargs 'console=ttyS0,115200 root=/dev/mmcblk0p2 rw rootwait'
setenv bootcmd 'mmc dev 0; fatload mmc 0:1 0x82000000 zImage; bootz 0x82000000 - 0x88000000'
saveenv
```

Or via `boot.scr` (generated from `boot.cmd`):

```bash
mkimage -C none -A arm -T script -d boot.cmd boot.scr
```

---

## 🧰 5. Debugging Tips

| Problem            | Check                                           |
| ------------------ | ----------------------------------------------- |
| No boot message    | Is MLO/U-Boot flashed at correct offset?        |
| Kernel panic       | Rootfs location or corrupt image                |
| Can't mount rootfs | Filesystem type? Root argument correct?         |
| NAND boot issues   | ECC settings in DTS/U-Boot                      |
| U-Boot not loading | UART logs, check voltages, check boot mode pins |

---

## ✅ Summary

| Media | Loader Location | Common Tooling             | Rootfs FS  |
| ----- | --------------- | -------------------------- | ---------- |
| eMMC  | Boot0/User area | `dd`, `fdisk`              | ext4       |
| SD    | First sectors   | `dd`, `mkfs.vfat`          | ext4       |
| NOR   | MTD partitions  | `mtd_debug`, `flash_erase` | cramfs/ubi |
| NAND  | MTD + UBI       | `nandwrite`, `ubiformat`   | ubifs      |

---

Would you like:

* A **script to prepare SD/eMMC bootable image**?
* A **device tree example** for NAND/NOR partitions?
* **U-Boot command sequence** for each storage medium?

Let me know!
