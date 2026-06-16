📦 Sample Initramfs Contents
=======================================================================================
Here’s a **sample initramfs image structure** and how to **build it from scratch** using **BusyBox**. This image is suitable for booting in QEMU or on an embedded board with basic UART, `/proc`, `/sys`, and shell support.

---

## 📦 Sample Initramfs Contents
=======================================================================================
```
initramfs/
├── bin/              → BusyBox binary (linked tools)
├── sbin/             → More symlinks to busybox
├── etc/
│   └── inittab       → Starts getty and shell
├── dev/              → Device nodes (null, console)
├── proc/             → Mount point for procfs
├── sys/              → Mount point for sysfs
├── tmp/              → Temporary runtime space
├── init              → Init script (entry point)
└── lib/              → Optional: Shared libs if not static
```

---

## 🔨 Steps to Create Sample Initramfs
=======================================================================================
### 1. **Build BusyBox (static)**

```bash
git clone git://busybox.net/busybox.git
cd busybox
make defconfig
make menuconfig
```

👉 Enable static build:

```
BusyBox Settings → Build Options → [*] Build static binary (no shared libs)
```

```bash
make -j4
make install CONFIG_PREFIX=../initramfs
```

---

### 2. **Create Init Script**

Create `initramfs/init`:

```sh
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev || mknod -m 622 /dev/console c 5 1
echo "Welcome to Embedded Linux"
exec /bin/sh
```

Make it executable:

```sh
chmod +x initramfs/init
```

---

### 3. **Create Essential Device Nodes**

```bash
sudo mknod -m 622 initramfs/dev/console c 5 1
sudo mknod -m 666 initramfs/dev/null c 1 3
```

---

### 4. **Pack into `initramfs.cpio.gz`**

```bash
cd initramfs
find . | cpio -o -H newc | gzip > ../initramfs.cpio.gz
```

---

### 5. **Run in QEMU (for ARM or x86)**

Example for x86:

```bash
qemu-system-x86_64 -kernel bzImage \
  -initrd initramfs.cpio.gz \
  -nographic -append "console=ttyS0"
```

For ARM:

```bash
qemu-system-arm -M virt -kernel zImage \
  -initrd initramfs.cpio.gz \
  -append "console=ttyAMA0" \
  -nographic
```

---

## 🧪 Test Output (Serial Console)
=======================================================================================
You should see:

```
Welcome to Embedded Linux
/ #
```

---

## 🧩 Optional: Add Networking, Scripts, Apps
=======================================================================================
To extend:

* Add `udhcpc`, `ifconfig`, `vi`, etc. in BusyBox
* Add `/etc/init.d/rcS` for script booting
* Add user accounts in `/etc/passwd` if needed

---

If you want a **ready-to-download tarball** or a QEMU-ready `initramfs.cpio.gz`, I can generate and share it as a file. Would you like that?
