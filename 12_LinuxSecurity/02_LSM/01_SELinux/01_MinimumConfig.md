Yes 👍 — that would be the **best way to actually learn SELinux in embedded**: start with a **tiny BusyBox rootfs**, label a file, write a **minimal policy**, compile it, and load it into the kernel.

Let me prepare that for you step-by-step.

---

# 🛠 Minimal SELinux Policy Example on BusyBox RootFS

## 1. Kernel prep

Enable SELinux in your kernel config:

```bash
CONFIG_SECURITY=y
CONFIG_SECURITY_SELINUX=y
CONFIG_SECURITY_SELINUX_BOOTPARAM=y
CONFIG_SECURITY_SELINUX_DISABLE=y
CONFIG_AUDIT=y
CONFIG_SECURITYFS=y
```

Boot with:

```bash
security=selinux selinux=1 enforcing=0
```

(so policy loads but is **permissive**).

---

## 2. Build a BusyBox RootFS

Make a small rootfs:

```bash
mkdir rootfs
cd rootfs
mkdir -p bin etc lib lib64 usr/sbin dev proc sys tmp
cp /path/to/busybox bin/
ln -s busybox bin/sh
```

---

## 3. Install SELinux tools into rootfs

On your build host (Ubuntu/Yocto), install:

```bash
sudo apt-get install selinux-basics selinux-utils policycoreutils checkpolicy
```

Copy necessary binaries (`sestatus`, `setenforce`, `restorecon`) into rootfs `/sbin`.

---

## 4. File labeling

Every file must have a SELinux label. For testing, just set labels manually:

```bash
chcon u:object_r:bin_t:s0 rootfs/bin/busybox
ls -Z rootfs/bin/busybox
```

---

## 5. Minimal policy (hello world)

Create `mydaemon.te`:

```te
policy_module(mydaemon, 1.0)

# Define a new type for our daemon
type mydaemon_t;
init_daemon_domain(mydaemon_t)

# Allow mydaemon to write logs
allow mydaemon_t var_log_t:file { create append write };
```

---

## 6. Compile and install policy

```bash
checkmodule -M -m -o mydaemon.mod mydaemon.te
semodule_package -o mydaemon.pp -m mydaemon.mod
semodule -i mydaemon.pp
```

Check installed modules:

```bash
semodule -l | grep mydaemon
```

---

## 7. Test in permissive mode

Run your daemon (could just be a simple BusyBox app like `echo` or a custom C program).

If it tries to write `/var/log/mydaemon.log` without permission, you’ll see AVC denials in logs:

```bash
dmesg | grep avc
```

Example:

```
avc: denied { write } for pid=123 comm="mydaemon" name="mydaemon.log" dev="mmcblk0p2" scontext=u:r:mydaemon_t:s0 tcontext=u:object_r:var_log_t:s0
```

---

## 8. Switch to enforcing

Once your rules are working:

```bash
setenforce 1
getenforce   # should return "Enforcing"
```

Now denials block access.

---

✅ That’s it — you now have a minimal working SELinux policy on a BusyBox rootfs.

---

👉 Do you want me to **write a tiny C “daemon” program** (just prints to `/var/log/hello.log`) and show you how to confine it under this custom SELinux domain — so you can test policy enforcement end-to-end?
