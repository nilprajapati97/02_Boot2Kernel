4. Root Filesystem (RootFS) Bring-Up – In Depth
=================================================================================================================
| Step              | Description                                                  |
| ----------------  | ------------------------------------------------------------ |
| ✅ BusyBox Init   | Use BusyBox or Yocto minimal image for first rootfs.         |
| ✅ Mount Points   | Check `/dev`, `/proc`, `/sys` mounts in `init` or `systemd`. |
| ✅ Serial Console | Ensure `getty` runs on console.                              |
| ✅ Shell Access   | Login prompt with working password and shell.                |
| ✅ Network Stack  | Enable DHCP/static IP, test `ping`, `wget`.                  |

The RootFS is the essential user-space portion of a Linux system. It contains everything outside the kernel: init system, shell, utilities, libraries, and config files. It's where the kernel hands over control during boot.

✅ 1. BusyBox Init / Minimal RootFS
=================================================================================================================

**What is it?**
    BusyBox combines small versions of many common UNIX utilities into a single binary. Ideal for embedded systems with limited storage.

**How to do it?**
    Build a minimal rootfs using BusyBox:
        bash:

            make defconfig
            make
            make install

This creates /bin/busybox, symlinks like /bin/ls, /bin/sh, etc.

**You’ll get a minimal FS like:**
            /bin
            /sbin
            /etc
            /dev
            /proc
            /sys
            /tmp

**Why use it?**
            Saves space.
            Easy to configure.
            Can serve as a fallback during bring-up.    

**📁 Directory Tree:**
            /init -> symlink to /bin/busybox or actual script
            /etc/inittab -> controls init behavior



✅ 2. Mount Points: /dev, /proc, /sys
=================================================================================================================
🔹 These are virtual filesystems:

    /dev: device nodes (created dynamically or statically)
    /proc: kernel process and system info (pseudo-files)
    /sys: system info via sysfs, exposes hardware hierarchy

🔹 Ensure they are mounted in init script:
    sh
    mount -t proc none /proc
    mount -t sysfs none /sys
    mount -t devtmpfs none /dev

🔹 Without them:
    ps, top, ifconfig, udev, and even boot scripts may break.
    Dynamic device creation (via udev or mdev) won’t work.


✅ 3. Serial Console: getty on /dev/ttyS0
=================================================================================================================

🔹 Purpose:
            Enables login prompt over serial for headless systems (common in embedded boards).
            Critical for debugging and root shell access.

🔹 Configurations:
        In /etc/inittab (SysV):
            T0:23:respawn:/sbin/getty -L ttyS0 115200 vt100
        In systemd:
            Enable serial console:
                bash:
                        systemctl enable serial-getty@ttyS0.service
🔹 Check bootargs:
        console=ttyS0,115200



✅ 4. Shell Access
=================================================================================================================
🔹 Goal:
        A login shell that works when the system boots.

🔹 Components:
            /etc/passwd, /etc/shadow, /etc/inittab
            /bin/sh (BusyBox provides this)

🔹 What to validate:
                    You can login using root with or without a password.
                    Shell prompt appears after login.
                    Filesystem is writable if needed (rw in bootargs or /etc/fstab).


✅ 5. Network Stack
=================================================================================================================
🔹 Required to bring up:
        Ethernet/Wi-Fi driver loaded correctly.
        Interface available (e.g., eth0 or wlan0).
        Network service like udhcpc or ifconfig + route.

🔹 Basic DHCP setup with BusyBox:
    sh:
       udhcpc -i eth0

🔹 Static IP setup:
    sh:
        ifconfig eth0 192.168.1.100 netmask 255.255.255.0
        route add default gw 192.168.1.1

🔹 Test:
    sh:
        ping 8.8.8.8
        wget http://example.com

🔹 Tools to include in rootfs:
                              ifconfig, ping, wget, netstat, udhcpc – all from BusyBox.


🧩 Debug Tips During RootFS Bring-Up
=================================================================================================================
| Symptom                 | Debug Hint                                      |
| ----------------------- | ----------------------------------------------- |
| No shell/login prompt   | Check getty, bootargs, or `init` script.        |
| Can't access `/dev`     | Confirm `devtmpfs` is mounted in init.          |
| `ping` fails            | Check MAC address, driver, `ifconfig`, routing. |
| Boot hangs after kernel | Check if rootfs is correctly passed (`root=`).  |
| "No init found" error   | Ensure `/init` or `/sbin/init` exists + exec.   |


📌 RootFS Can Be Provided As:
=================================================================================================================

**Initramfs** (embedded in kernel or passed by bootloader).
**initrd** (temporary fs loaded by bootloader, later replaced).
**SD/eMMC/USB/NFS rootfs** – mounted over appropriate bus.

