Great question 👌 — now let’s go through **AppArmor in Embedded Linux security**, step by step **from scratch**, so you see how it works inside the Linux kernel and why it’s sometimes chosen over SELinux in embedded devices.

---

# 🛡️ AppArmor in Embedded Linux Security

---

## 1. What is AppArmor?

* **AppArmor = Linux Security Module (LSM)**, like SELinux, TOMOYO, Smack.
* Provides **Mandatory Access Control (MAC)**, but **profile-based** rather than **label-based** (SELinux).
* Instead of labeling every file/device, AppArmor enforces **rules based on file paths**.

👉 This makes AppArmor **easier to use** in embedded systems where you control the filesystem layout and don’t want the complexity of SELinux labeling.

---

## 2. Kernel integration

* Kernel must be built with:

  ```bash
  CONFIG_SECURITY=y
  CONFIG_SECURITY_APPARMOR=y
  CONFIG_SECURITYFS=y
  ```
* At boot, enable with:

  ```
  apparmor=1 security=apparmor
  ```
* Profiles are loaded into the kernel via `/sys/kernel/security/apparmor/` or user tools (`apparmor_parser`).

---

## 3. How AppArmor works

* Each **program** has a **profile**.
* Profile defines **what files, network, capabilities** the program can use.
* Enforcement is **per executable path** (e.g., `/usr/bin/mydaemon`).

**Example profile:**

```
/usr/sbin/mydaemon {
  # Allow read-only access to config
  /etc/mydaemon.conf r,

  # Allow creating/writing log
  /var/log/mydaemon.log rw,

  # Allow network client
  network inet stream,
}
```

* If `mydaemon` tries to read `/etc/shadow`, AppArmor blocks it (even as root).

---

## 4. Modes of AppArmor

1. **Enforce mode** → Denials are enforced + logged.
2. **Complain mode** → Violations are logged but allowed (good for testing).

Switch mode:

```bash
aa-complain /usr/sbin/mydaemon
aa-enforce /usr/sbin/mydaemon
```

---

## 5. Embedded Linux use-case

Why AppArmor is attractive for embedded:

* ✅ **Simplicity**: No need for complex SELinux labeling (xattrs).
* ✅ **Path-based rules**: Easy to write/edit text profiles.
* ✅ **Low footprint**: Good for BusyBox/Yocto-based systems.
* ✅ **Easier maintenance**: OEMs with small security teams can manage it.

Downside:

* ❌ Path-based → possible bypasses (hardlinks, renamed files).
* ❌ Less fine-grained than SELinux (no type enforcement model).
* ❌ Less common in Android/Automotive (they prefer SELinux/Smack).

---

## 6. AppArmor vs SELinux in Embedded

| Feature      | SELinux                          | AppArmor                        |
| ------------ | -------------------------------- | ------------------------------- |
| Policy model | Label-based (type enforcement)   | Path-based profiles             |
| Complexity   | High (labels, policy modules)    | Low (simple text rules)         |
| Footprint    | Larger (needs xattrs, policy DB) | Smaller                         |
| Embedded use | Android, Automotive IVI          | IoT, Ubuntu Core, small devices |
| Maintenance  | Complex, needs tooling           | Easier, profiles editable       |

---

## 7. Workflow in Embedded RootFS

### Step 1 — Enable AppArmor in kernel

```bash
CONFIG_SECURITY_APPARMOR=y
CONFIG_DEFAULT_SECURITY_APPARMOR=y
```

### Step 2 — Install tools

On host:

```bash
apt-get install apparmor apparmor-utils
```

### Step 3 — Write a profile

File: `/etc/apparmor.d/usr.sbin.mydaemon`

```
# Profile for mydaemon
/usr/sbin/mydaemon {
   /etc/mydaemon.conf r,
   /var/log/hello.log rw,
   /dev/null rw,
   network inet stream,
}
```

### Step 4 — Load profile

```bash
apparmor_parser -r -W /etc/apparmor.d/usr.sbin.mydaemon
```

### Step 5 — Run and test

* Run daemon, try to access allowed file → works.
* Try to access `/etc/shadow` → denied:

  ```
  apparmor="DENIED" operation="open" profile="/usr/sbin/mydaemon" name="/etc/shadow"
  ```

---

## 8. Where AppArmor is used

* **Ubuntu Core (embedded IoT OS)** → AppArmor is the default confinement mechanism.
* **Snap packages** use AppArmor for isolation.
* **Smaller embedded Linux** distributions sometimes prefer AppArmor over SELinux because it doesn’t require relabeling filesystems.

---

✅ **Summary**

* AppArmor = LSM enforcing MAC with **per-executable profiles**.
* Easier than SELinux → no labels, just path-based rules.
* Good fit for **IoT / embedded devices** with small teams, less complex policies.
* Kernel needs to enable `CONFIG_SECURITY_APPARMOR`.
* Profiles loaded via `/etc/apparmor.d/` and enforced by kernel.

---

👉 Do you want me to prepare a **minimal AppArmor profile + test example** (with a simple C daemon writing logs, like we did for SELinux), so you can directly compare AppArmor vs SELinux hands-on?
