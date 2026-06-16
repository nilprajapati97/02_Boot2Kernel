🔐 7. Security & Production Prep
=======================================================================================
| Step                       | Description                                            |
| ---------------------------|------------------------------------------------------- |
| ✅ Remove Debug Ports      | Disable console, JTAG, unused peripherals.             |
| ✅ Secure Boot             | Sign U-Boot and kernel. Enable signature verification. |
| ✅ Read-only RootFS        | Use SquashFS or overlayfs for root filesystem.         |
| ✅ udev Rules              | Set up persistent device naming.                       |
| ✅ Logging                 | Enable system logs, rotate and store logs.             |
| ✅ Production Flash Layout | Define bootloader, kernel, rootfs partitions clearly.  |

Here's a **deep dive explanation** of each item under 🔐 **Security & Production Prep** specifically for a **custom embedded Linux board** (like an IoT gateway, HMI panel, industrial controller, etc.):

---

## 🔐 7. Security & Production Prep (In Depth for Custom Embedded Linux Board)
====================================================================================================================================

| Step | In-Depth Description |
| ---- | -------------------- |

### ✅ 1. Remove Debug Ports
====================================================================================================================================

**Goal:** Prevent unauthorized low-level access to hardware and software.

#### 🔧 Actions:

* **Disable UART Console:**
  Remove `console=ttyS0` or similar from kernel command line. Rebuild U-Boot or kernel to disable early debug prints.
* **JTAG Disable:**

  * Configure SoC pinmux to disable JTAG/SWD.
  * In production fuses/eFuses may be used to permanently disable JTAG.
* **USB Debug Off:**
  Disable g\_serial or g\_dbg on USB gadget mode in production kernel.
* **Other Interfaces:**
  Disable unused SPI/I2C/GPIO interfaces if not needed.

#### ✅ Outcome:

* Prevent hackers from attaching probes and accessing bootloader or shell access.

---

### ✅ 2. Secure Boot
====================================================================================================================================

**Goal:** Ensure that only verified, trusted software runs on your device.

#### 🔧 Actions:

* **Bootloader Signing:**

  * Use SoC ROM code to verify signed U-Boot (e.g., using RSA keys).
  * Fuse the public key into SoC or load from secure flash.
* **Kernel/Image Signing:**

  * Enable U-Boot FIT image with hash and signature verification.
  * Configure U-Boot to verify FIT images using embedded public keys.
* **Root of Trust Setup:**

  * Leverage hardware fuses/OTP for storing hash of signing key.

#### ✅ Outcome:

* Stops booting of tampered U-Boot or kernel images (anti-malware baseline).

---

### ✅ 3. Read-Only RootFS
====================================================================================================================================

**Goal:** Prevent tampering of OS files and ensure system integrity.

#### 🔧 Options:

* **SquashFS**

  * Compressed, read-only filesystem. Ideal for embedded systems.
  * Used for `/` or `/usr`.
* **OverlayFS**

  * Union of read-only (SquashFS) and read-write (tmpfs or persistent data partition).
  * Writable paths like `/etc`, `/var`, `/home` can overlay a read-only base.
* **Mount with `ro`**

  * Mount root with `ro` flag via kernel cmdline.

#### ✅ Outcome:

* Reduces attack surface.
* Any malware changes vanish on reboot if not in persistent partition.

---

### ✅ 4. udev Rules
====================================================================================================================================

**Goal:** Consistent and secure device naming for peripherals.

#### 🔧 Actions:

* Create `/etc/udev/rules.d/99-custom.rules`

  * Example:

    ```
    SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", NAME="gps0"
    ```
* Use rules to control permissions (`MODE=0660`, `GROUP=gps`)
* Prevent dynamic assignment of devices like `/dev/ttyUSB0` changing on boot.

#### ✅ Outcome:

* Stability in device access logic (esp. for production software)
* Can control who accesses devices (important for security).

---

### ✅ 5. Logging
====================================================================================================================================

**Goal:** Monitor system behavior, failures, or intrusion attempts.

#### 🔧 Actions:

* **Enable Logging:**

  * Use `rsyslog`, `logrotate`, or `journald`.
* **Log Rotation:**

  * Prevent `/var/log` from filling flash storage.
  * `logrotate.conf` can define max size and rotation frequency.
* **Secure Logging:**

  * Set logs as read-only for non-root.
  * Encrypt sensitive logs or forward to a secure server.

#### ✅ Outcome:

* Enables diagnosis of bugs or security events without compromising system stability.

---

### ✅ 6. Production Flash Layout
====================================================================================================================================

**Goal:** Clear partitioning for reliability and OTA updates.

#### 🔧 Sample Layout:

| Partition    | Description                     |
| ------------ | ------------------------------- |
| `bootloader` | U-Boot or SPL                   |
| `boot`       | Kernel + DTB (may be FIT image) |
| `rootfs_a`   | Read-only squashfs, version A   |
| `rootfs_b`   | For OTA fallback                |
| `data`       | Persistent config, user data    |
| `logs`       | Rotating logs (optional)        |
| `env`        | U-Boot environment              |

#### ✅ Outcome:

* Supports A/B updates
* Ensures rootfs rollback if upgrade fails
* Keeps configuration and logs safe across reboots

---

## Summary Table
====================================================================================================================================

| 🔐 Area          | ✅ Security Impact                      |
| ---------------- | -------------------------------------- |
| Debug Ports      | Prevents unauthorized low-level access |
| Secure Boot      | Stops unsigned/tampered software       |
| Read-Only RootFS | Prevents persistent tampering          |
| udev Rules       | Stable & secure peripheral handling    |
| Logging          | Auditing and diagnostics               |
| Flash Layout     | Robust, upgrade-safe system design     |

---

Would you like example U-Boot config for Secure Boot or a full flash layout diagram?
====================================================================================================================================
Great! Below are **both**:

---

## ✅ Example: U-Boot Secure Boot Configuration

This example assumes you're using **FIT image** (Flattened Image Tree) format and **RSA signature** support in U-Boot.

### 🔹 1. Generate Keys (host side)

```bash
# Private key (keep this secure)
openssl genpkey -algorithm RSA -out dev.key -pkeyopt rsa_keygen_bits:2048

# Public key (embed into U-Boot)
openssl rsa -in dev.key -pubout -out dev_pub.key
```

---

### 🔹 2. FIT Image Source `.its`

Create a `.its` file to define kernel, DTB, and signature layout:

```dts
/dts-v1/;

/ {
    description = "Secure Boot Image";

    images {
        kernel {
            description = "Linux Kernel";
            data = /incbin/("zImage");
            type = "kernel";
            arch = "arm";
            os = "linux";
            compression = "none";
            load = <0x80008000>;
            entry = <0x80008000>;
            hash {
                algo = "sha256";
            };
        };

        fdt {
            description = "Flattened Device Tree";
            data = /incbin/("your_board.dtb");
            type = "flat_dt";
            arch = "arm";
            compression = "none";
            hash {
                algo = "sha256";
            };
        };
    };

    signatures {
        kernel_sig {
            algo = "rsa2048";
            key-name-hint = "dev";
            sign-images = "kernel", "fdt";
        };
    };
};
```

---

### 🔹 3. Create and Sign FIT Image

```bash
mkimage -f image.its -k keys_dir -K u-boot.dtb -r fitImage
```

* `-k keys_dir`: Directory containing `dev.key` and `dev_pub.key`
* `-K u-boot.dtb`: Embed public key into U-Boot’s DTB (rebuild U-Boot with signature verification enabled)

---

### 🔹 4. U-Boot Configuration

**In defconfig:**

```bash
CONFIG_FIT=y
CONFIG_FIT_SIGNATURE=y
CONFIG_RSA=y
CONFIG_OF_CONTROL=y
CONFIG_DEFAULT_DEVICE_TREE="your_board"
```

**U-Boot Environment:**

```bash
setenv bootargs console=ttyS0,115200 root=/dev/mmcblk0p2 ro
load mmc 0:1 ${kernel_addr_r} fitImage
bootm ${kernel_addr_r}
```

U-Boot will **reject the boot** if the signature check fails. ✅

---

## 🧱 Full Flash Layout Diagram (Typical for NAND/eMMC/SD-based board)

```
-------------------------------------------------------
| Partition       | Size      | Content               |
|-----------------|-----------|------------------------|
| Bootloader      | 1MB       | SPL + U-Boot           |
| U-Boot Env      | 256KB     | u-boot.env (optional)  |
| Boot (FIT Img)  | 8MB       | Kernel + DTB + Sig     |
| RootFS_A        | ~512MB    | SquashFS (RO)          |
| RootFS_B        | ~512MB    | Alternate rootfs (RO)  |
| Data            | ~256MB    | /etc, /home, config    |
| Logs            | ~128MB    | /var/log               |
| Reserved OTA    | ~256MB    | update package buffer  |
-------------------------------------------------------
```

### 🔸 Key Notes:

* **RootFS\_A/RootFS\_B**: Used for A/B upgrade schemes.
* **Boot partition** contains `fitImage`.
* **U-Boot Env** can be backed up (`env_redundant`) for safety.
* **Data/logs** are `ext4` or `ubifs`, writable partitions.

---

Would you like a sample **A/B update logic in U-Boot script** or a full **init script to mount overlayfs**?
