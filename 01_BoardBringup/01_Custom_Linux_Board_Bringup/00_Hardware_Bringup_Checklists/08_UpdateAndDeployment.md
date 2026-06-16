🔄 8. Update & Deployment – In Depth for Custom Embedded Linux Board
======================================================================================
| Step                         | Description                                          |
| ---------------------------  | ---------------------------------------------------- |
| ✅ A/B System or Backup Boot | Add redundancy for kernel/rootfs in case of failure. |
| ✅ OTA (Optional)            | Plan or integrate Over-The-Air update system.        |
| ✅ Recovery Mode             | Setup UMS or DFU mode fallback for rescue.           |


Here’s a **deep-dive explanation** of 🔄 **Update & Deployment** for a **custom embedded Linux board**, covering production-grade reliability, OTA (Over-The-Air), and recovery options.

---

## 🔄 8. Update & Deployment – In Depth for Custom Embedded Linux Board

| Step | In-Depth Explanation |
| ---- | -------------------- |

---

### ✅ 1. A/B System or Backup Boot
====================================================================================================================================
**Goal:** Ensure the system can recover from a failed update (kernel/rootfs).

#### 🔧 Concept:

* Use two sets of kernel + rootfs partitions: `A` (active) and `B` (backup/future).
* Bootloader (U-Boot) chooses A or B based on environment variable or boot success flags.

#### 🧱 Flash Partitioning Example:

```
| Partition      | Description               |
|----------------|---------------------------|
| boot_a         | Kernel + DTB (FIT)        |
| rootfs_a       | SquashFS or ext4 (RO)     |
| boot_b         | Backup Kernel             |
| rootfs_b       | Backup RootFS             |
| data           | Writable config/logs      |
| ota_flag       | Boot slot info, status    |
```

#### 🔁 Boot Logic in U-Boot:

```bash
# Pseudocode logic in U-Boot
if test ${upgrade_available} = 1; then
  if test ${boot_slot} = B; then
    setenv boot_part boot_b
    setenv rootfs_part rootfs_b
  else
    setenv boot_part boot_a
    setenv rootfs_part rootfs_a
  fi
else
  # fallback logic if boot fails
  run boot_backup
fi

# Boot command
load mmc 0:${boot_part} ${kernel_addr_r} fitImage
bootm ${kernel_addr_r}
```

#### ✅ Outcome:

* If A fails, fall back to B
* Ensures device is always bootable (very important for remote devices)

---

### ✅ 2. OTA (Over-The-Air) Update System (Optional)
====================================================================================================================================
**Goal:** Update system software remotely with rollback safety.

#### 🔧 OTA System Components:

* **Update Agent:**
  Runs on device (`swupdate`, `RAUC`, `Mender`, or custom script)
* **Update Server:**
  Hosts signed images and metadata.
* **Secure Transport:**
  Typically HTTPS or MQTT with auth.
* **Package Format:**
  `*.swu`, signed tarball, or FIT image with multi-components.

#### 🛠️ OTA Flow Example:

```bash
1. Device checks update server.
2. Downloads update package securely.
3. Verifies signature (hash + GPG/RSA).
4. Flashes to inactive slot (e.g., rootfs_b).
5. Sets U-Boot env: boot_slot=B, upgrade_available=1
6. Reboots into new slot.
7. Application verifies new boot is stable.
8. If stable: sets upgrade_available=0
   Else: reboot and fallback to previous slot.
```

#### 🔐 Security Practices:

* Sign update packages.
* Use TLS with device authentication.
* Store upgrade flags in a separate partition or persistent U-Boot env.

---

### ✅ 3. Recovery Mode
====================================================================================================================================
**Goal:** Provide a fail-safe recovery interface if all boots fail.

#### 🔧 Options:

1. **USB Mass Storage Mode (UMS)**
   U-Boot exposes internal eMMC/NAND as a USB drive to host PC.

   ```bash
   ums 0 mmc 0
   ```

   > Host PC can copy new boot/rootfs images.

2. **DFU Mode (Device Firmware Upgrade)**
   USB protocol for flashing via host.

   ```bash
   dfu 0 mmc 0
   ```

3. **Recovery Partition with Minimal System**
   Tiny rootfs (BusyBox + UMS or network tools).
   Used to recover via USB, UART, or SSH.

4. **Boot Key Trigger (GPIO or UART Input)**

   * Hold button during boot to enter recovery.
   * Or wait for UART input: e.g., type `RECOVERY`.

#### 🧱 Flash Layout Example with Recovery:

```
| bootloader      |
| boot_a / boot_b |
| rootfs_a / b    |
| recovery_rootfs | <- read-only, always bootable
| data            |
```

---

## 🧩 Summary Table
====================================================================================================================================
| Feature       | Role in Reliability & Maintenance                  |
| ------------- | -------------------------------------------------- |
| A/B Boot      | Ensures one known-good system is always available  |
| OTA           | Enables remote upgrades for large deployments      |
| Recovery Mode | Allows field engineers or users to recover devices |

---




