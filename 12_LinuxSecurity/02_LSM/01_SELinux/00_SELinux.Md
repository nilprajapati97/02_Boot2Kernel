Got it ✅ — let’s build **SELinux in Embedded Linux** step by step, **from scratch**, so you can see the big picture and the actual mechanics.

---

# 🔒 SELinux in Embedded Linux — From Scratch

---

## 1. Why do we even need SELinux?

* Traditional Linux uses **DAC (Discretionary Access Control)** → file permissions (`rwx` bits) + root can do anything.
* Embedded devices (phones, routers, IVI, IoT) need **stronger isolation** because:

  * You don’t want a compromised process (say, media player) to access sensitive files (like modem firmware).
  * You want even root processes restricted unless the *policy* allows it.
* **SELinux = Mandatory Access Control (MAC)** → rules are enforced by the kernel, *not bypassable even by root*.

---

## 2. What is SELinux really?

* **Security Enhanced Linux** = an **LSM (Linux Security Module)** implementing **Type Enforcement** + **RBAC** + **MLS (Multi-Level Security)**.
* Every **process** (subject) and every **object** (file, socket, device node, etc.) is assigned a **security context** (a label).
* Kernel checks **policy rules**:
  “Can subject with type `foo_t` access object with type `bar_t` using operation `read/write/execute`?”

---

## 3. SELinux Security Context

A context looks like:

```
user:role:type:level
```

Example:

```
u:r:init:s0
u:r:hal_camera_default:s0
```

* `user` → SELinux user (not Linux login user).
* `role` → defines what types can be accessed.
* `type` → MOST IMPORTANT. Defines domain for process or label for object. (e.g., `init_t`, `system_server_t`).
* `level` → MLS/MCS (e.g., `s0`, `s0:c123,c456`).

👉 In **Embedded Linux (e.g., Android)**, **type enforcement (TE)** is the main mechanism.

---

## 4. How SELinux is integrated in the kernel

* Kernel config:

  ```bash
  CONFIG_SECURITY=y
  CONFIG_SECURITY_SELINUX=y
  CONFIG_SECURITYFS=y
  ```
* At boot: kernel loads **SELinux policy** (usually from `/etc/selinux/` or in Android `/sepolicy`).
* Filesystems must support **xattrs** (e.g., `ext4`, `f2fs`).
* Each file has `security.selinux` extended attribute storing its label.

---

## 5. Modes of SELinux

1. **Enforcing** → policy rules are enforced, denials logged.
2. **Permissive** → violations are logged but allowed.
3. **Disabled** → SELinux off.

Check mode:

```bash
getenforce
cat /sys/fs/selinux/enforce
```

---

## 6. How policy works in Embedded Linux

### Policy = Set of rules

* Defines **what domain (process type) can access what object type** and how.
* Example rule:

```te
allow hal_camera_default camera_device:chr_file { open read write ioctl };
```

Meaning:

* Process in domain `hal_camera_default`
* Can `open, read, write, ioctl`
* On `camera_device` character device node.

---

## 7. SELinux in Android (embedded example)

* Android adopted **SELinux Enforcing** since Android 5 (Lollipop).
* Example:

  * `system_server` → runs in `system_server_t` domain.
  * `mediaserver` → runs in `media_server_t` domain.
  * Policy explicitly says what system\_server can talk to.
  * If an exploit hijacks mediaserver, it cannot touch `/dev/radio` because policy denies it.
* This is why **Android exploits often get blocked by SELinux denials**.

---

## 8. Workflow for Embedded Developer

### Step 1 — Enable SELinux in kernel

* Add configs (`CONFIG_SECURITY_SELINUX`)
* Add `security=selinux selinux=1 enforcing=0` to bootargs for testing.

### Step 2 — Prepare RootFS with SELinux tools

Install:

* `libselinux`
* `policycoreutils` (contains `setenforce`, `sestatus`, `restorecon`)
* `checkpolicy` and `policycoreutils` for building policy

### Step 3 — Label files

* Filesystems need labels. Example:

```bash
chcon u:object_r:system_file:s0 /bin/busybox
ls -Z /bin/busybox
```

### Step 4 — Write Policy

Minimal example `mydaemon.te`:

```te
type mydaemon_t;
allow mydaemon_t self:capability { net_bind_service };
allow mydaemon_t var_log_t:file { create read write };
```

Compile and load:

```bash
checkmodule -M -m -o mydaemon.mod mydaemon.te
semodule_package -o mydaemon.pp -m mydaemon.mod
semodule -i mydaemon.pp
```

### Step 5 — Test in permissive mode

* Run daemon, check logs in `dmesg` or `/var/log/audit/audit.log`.
* Example denial:

```
avc: denied { read } for pid=123 comm="mydaemon" name="config" dev="mmcblk0p2" ino=1234 scontext=u:r:mydaemon:s0 tcontext=u:object_r:etc_t:s0 tclass=file
```

* Use `audit2allow` to generate rules:

```bash
audit2allow -a
```

### Step 6 — Switch to enforcing

```bash
setenforce 1
```

---

## 9. SELinux in Automotive / IoT

* **Android Automotive OS (AAOS)** → uses SELinux just like phones.
* **GENIVI Linux / AGL** → often use **Smack** instead of SELinux (simpler).
* **Tiny IoT Linux** → may prefer AppArmor or TOMOYO, because SELinux policies are heavy.

---

## 10. Benefits for Embedded

* ✅ Contain apps/drivers (compromised media HAL cannot access modem HAL).
* ✅ Enforce least privilege on daemons (each HAL runs in its own domain).
* ✅ Protect kernel attack surface (policy denies dangerous `ioctl`).
* ✅ Prevent privilege escalation (root is not “god” anymore).

---

📌 **Summary**

* SELinux = LSM enforcing MAC via labeling.
* Needs **kernel support, xattrs, policies**.
* In embedded (esp. Android) → used to isolate HALs, services, apps.
* Development cycle: start **permissive**, collect **denials**, refine policy, then switch **enforcing**.

---

Do you want me to **create a minimal working SELinux policy + file labeling example** (with code and commands) for an embedded BusyBox-based rootfs, so you can actually test on QEMU or your board?
