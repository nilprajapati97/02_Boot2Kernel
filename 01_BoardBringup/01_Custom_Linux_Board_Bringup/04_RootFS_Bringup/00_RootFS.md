📦 4. Root Filesystem (RootFS)
====================================================================================
| Step              | Description                                                  |
| ----------------  | ------------------------------------------------------------ |
| ✅ BusyBox Init   | Use BusyBox or Yocto minimal image for first rootfs.         |
| ✅ Mount Points   | Check `/dev`, `/proc`, `/sys` mounts in `init` or `systemd`. |
| ✅ Serial Console | Ensure `getty` runs on console.                              |
| ✅ Shell Access   | Login prompt with working password and shell.                |
| ✅ Network Stack  | Enable DHCP/static IP, test `ping`, `wget`.                  |



Here's a **step-by-step, in-depth explanation of Root Filesystem (RootFS)** setup and bring-up for an Embedded Linux board (e.g., Qualcomm SDM660 or any ARM-based SoC):

---

## 📦 4. Root Filesystem (RootFS) Bring-Up
=======================================================================================
A working Linux system needs a valid **root filesystem** mounted during kernel boot. This contains init system, shell, drivers (modules), configs, and basic userspace tools.

---

### ✅ Step 1: BusyBox Init
=======================================================================================
#### 🔸 What is BusyBox?

BusyBox is a single binary that provides many standard Unix utilities (ls, sh, mount, etc.) and is ideal for minimal embedded environments.

#### 🧩 How to Use:

1. **Download and Configure:**

   ```sh
   git clone git://busybox.net/busybox.git
   cd busybox
   make defconfig
   make menuconfig     # Optional: Enable applets you want
   ```

2. **Build:**

   ```sh
   make -j4
   make install        # Installs in _install/ directory
   ```

3. **Create Directory Structure:**

   ```
   rootfs/
   ├── bin -> busybox
   ├── sbin -> busybox
   ├── etc/
   ├── dev/
   ├── proc/
   ├── sys/
   ├── tmp/
   ├── var/
   ├── init -> /bin/busybox
   ```

4. **Init File:**

   * Create an `init` script under `/init` or `/etc/init.d/rcS`:

     ```sh
     #!/bin/sh
     mount -t proc none /proc
     mount -t sysfs none /sys
     echo "Starting shell..."
     exec /bin/sh
     ```

5. **Build RootFS Image:**

   ```sh
   cd rootfs
   find . | cpio -o -H newc | gzip > ../rootfs.cpio.gz
   ```

---

### ✅ Step 2: Mount Points - `/proc`, `/sys`, `/dev`
=======================================================================================
#### 🔸 Purpose:

These are virtual filesystems for kernel-user communication.

* `/proc`: Kernel state and process info
* `/sys`: Exposes device tree and drivers
* `/dev`: Device nodes like `/dev/ttyS0`, `/dev/mmcblk0`

#### 🔸 How to Set in `init`:

```sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
```

Make sure:

* Kernel has `CONFIG_DEVTMPFS=y`
* Mount `/dev` early or udev won’t work

---

### ✅ Step 3: Serial Console Setup (`getty`)
=======================================================================================
#### 🔸 What is `getty`?

`getty` provides the login prompt on the serial port.

#### 🛠️ Add to `/etc/inittab`:

```sh
::sysinit:/etc/init.d/rcS
ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100
```

#### 🔸 Device Tree:

Ensure UART is enabled in DTS:

```dts
&uart1 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&uart1_pins>;
};
```

---

### ✅ Step 4: Shell Access
=======================================================================================
You should now see:

```sh
Welcome to Embedded Linux
/ # 
```

If you don't get prompt:

* Check if BusyBox has `sh`
* Check if getty is spawning on correct TTY
* Confirm kernel bootargs: `console=ttyS0,115200`

---

### ✅ Step 5: Network Stack (DHCP, Ping)
=======================================================================================
#### 🧩 Kernel Config:

Ensure kernel has:

* `CONFIG_NET`
* `CONFIG_INET`
* `CONFIG_DHCP`
* `CONFIG_NETFILTER`

#### 🔧 Interfaces Setup:

* Static IP:

  ```sh
  ifconfig eth0 192.168.1.100 up
  route add default gw 192.168.1.1
  ```
* DHCP:

  ```sh
  udhcpc -i eth0
  ```

#### ✅ Test:

```sh
ping 8.8.8.8
wget http://example.com
```

---

## 🔍 Debug Tips
=======================================================================================
| Problem             | Fix                                                        |
| ------------------- | ---------------------------------------------------------- |
| No shell prompt     | Check `/init` or `/etc/inittab`, serial console config     |
| No device nodes     | Ensure `mount -t devtmpfs`, check `CONFIG_DEVTMPFS`        |
| Network not working | Check MAC address, PHY config, try `ifconfig` or `ethtool` |
| Cannot ping IPs     | Check IP routes via `route`, make sure interface is `up`   |
| No `getty` login    | Validate `/etc/inittab`, check device tree UART status     |

---

## 📦 Summary Diagram
=======================================================================================
```plaintext
+---------------------------+
|        Kernel Boots       |
+---------------------------+
            ↓
+---------------------------+
|    Mount RootFS (CPIO)    |
+---------------------------+
            ↓
+---------------------------+
|      Init or /init        |
|   → mount /proc, /sys     |
|   → mount /dev            |
|   → start /bin/sh         |
+---------------------------+
            ↓
+---------------------------+
|  getty spawns shell       |
|  (console login prompt)   |
+---------------------------+
            ↓
+---------------------------+
| Network up: ping, wget    |
| via udhcpc or static IP   |
+---------------------------+
```

---

If you want, I can provide:

* Sample `initramfs` image
* Real serial logs from QEMU or SDM660
* Minimal Yocto or Buildroot integration

Let me know!
