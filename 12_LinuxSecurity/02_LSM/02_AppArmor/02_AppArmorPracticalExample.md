Great — let’s do the **step-by-step, from scratch, in-depth AppArmor guide** with a practical example: a tiny C daemon that writes to `/var/log/hello.log`, an AppArmor profile to confine it, and all commands you need to build, install, test and debug on a BusyBox/QEMU or embedded rootfs. I’ll explain *why* each step matters so you learn the mechanics, not just the copy/paste.

# 🔰 Quick overview

* AppArmor is a profile-based LSM: policies bind to executable **paths** and state what file/network/capability operations the program may do.
* We’ll:

  1. build a tiny static C daemon,
  2. prepare a BusyBox rootfs,
  3. add a minimal AppArmor profile,
  4. load the profile and test in **complain** and **enforce** modes,
  5. debug denials and harden the profile.

---

# 1) Prerequisites (host + kernel)

**What you need on host**

* `gcc` (or cross-toolchain for target arch), `make` optional.
* AppArmor userland tools: `apparmor-utils`, `apparmor` (on Ubuntu: `sudo apt install apparmor apparmor-utils apparmor-parser`).
* `busybox` for small rootfs (or your own embedded rootfs).

**Kernel support (target)**

* Kernel must be built with AppArmor:

```
CONFIG_SECURITY=y
CONFIG_SECURITY_APPARMOR=y
CONFIG_SECURITYFS=y
```

* Boot kernel with `security=apparmor` (optional if built as default). If AppArmor not enabled at boot you’ll get no enforcement.
* Check active LSMs on a running system:

```bash
cat /sys/kernel/security/lsm
# usually shows: "lockdown,yama,apparmor" or similar when AppArmor is enabled
```

---

# 2) The tiny C daemon (code + compile)

Create `mydaemon.c` — a minimal logger that appends timestamps to `/var/log/hello.log` every 5 seconds:

```c
// mydaemon.c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    FILE *fp;
    int i = 0;
    while (1) {
        fp = fopen("/var/log/hello.log", "a");
        if (!fp) {
            perror("fopen");
            return 1;
        }
        time_t now = time(NULL);
        fprintf(fp, "mydaemon: log entry %d at %s", i++, ctime(&now));
        fclose(fp);
        sleep(5);
    }
    return 0;
}
```

**Compile (native, static recommended for small rootfs):**

```bash
# static build to avoid missing libc on target
gcc -static -O2 -o mydaemon mydaemon.c
```

If target arch ≠ host, cross-compile with your toolchain (e.g. `arm-linux-gnueabihf-gcc ...`).

Why static? Simple rootfs often lacks all shared libraries — static binary reduces runtime dependencies. If you prefer dynamic, ensure libraries exist in the rootfs.

---

# 3) BusyBox rootfs setup (minimal)

Create a minimal rootfs layout:

```bash
mkdir -p rootfs/{bin,sbin,usr/sbin,etc,var/log,proc,sys,dev,tmp}
cp busybox rootfs/bin/           # or symlink if busybox on host
cp mydaemon rootfs/usr/sbin/
chmod +x rootfs/usr/sbin/mydaemon
```

Create device nodes (if testing on QEMU):

```bash
sudo mknod rootfs/dev/null c 1 3
sudo mknod rootfs/dev/console c 5 1
# set permissions
sudo chmod 666 rootfs/dev/null
```

(You can use `genext2fs`/`cpio`/`mkfs` to make an image for QEMU.)

---

# 4) AppArmor profile — minimal and explicit

AppArmor profiles are plain text and live under `/etc/apparmor.d/`. Profiles bind to an executable path. Create `/etc/apparmor.d/usr.sbin.mydaemon` with:

```text
# /etc/apparmor.d/usr.sbin.mydaemon
#include <tunables/global>

/usr/sbin/mydaemon {
    # basic read access to config (if any)
    /etc/mydaemon.conf r,

    # allow read/write/create on the specific log file
    /var/log/hello.log rw,

    # allow writing to /dev/null
    /dev/null rw,

    # allow program to read libraries if dynamic (uncomment if dynamic)
    # /lib/** r,
    # /usr/lib/** r,

    # allow basic network client usage (optional)
    # network inet stream,

    # deny everything else by omission
}
```

Key points:

* Profile name is the canonical path to the executable (`/usr/sbin/mydaemon`) — AppArmor matches executed binaries by path.
* The `#include <tunables/global>` brings common variables like `%h` (home).
* Path rules are explicit and comma/space separated; `rw` is read/write, `r` read only.
* Omitted operations are implicitly denied in enforce mode.

---

# 5) Loading the profile & profile modes

**Install the profile on target rootfs** (copy this file into `rootfs/etc/apparmor.d/` before boot or place into running system's `/etc/apparmor.d/`).

**Load / replace profile:**

```bash
# parse and load (replace existing if any)
sudo apparmor_parser -r /etc/apparmor.d/usr.sbin.mydaemon
```

**Check status:**

```bash
sudo aa-status
# shows loaded profiles and modes
```

**Set profile to complain (log-only) or enforce:**

```bash
# complain = log but allow (good for policy tuning)
sudo aa-complain /usr/sbin/mydaemon

# enforce = block disallowed ops
sudo aa-enforce /usr/sbin/mydaemon
```

Why complain first? Complain mode lets the program run while you capture what would have been denied — you can then adjust the profile before switching to enforce.

---

# 6) Run and test (complain → tune → enforce)

Start the daemon on target (or chroot into rootfs):

```bash
# on target system
/usr/sbin/mydaemon &
```

**Allowed test:** ensure `/var/log/hello.log` is created and gets appended:

```bash
tail -f /var/log/hello.log
# you should see "mydaemon: log entry 0 ..." every 5 sec
```

**Denied test:** try to open a protected file (e.g. `/etc/shadow`) from the daemon — easiest approach: temporarily modify the daemon to open `/etc/shadow`, or use a small test program. But you can also validate denial by manually accessing via a process confined by the profile.

**Where are denials logged?**
AppArmor denial messages go to kernel log / syslog / journal. Examples:

```bash
# view kernel messages (works on both syslog and systemd-journal systems)
dmesg | tail -n 50
# or
sudo journalctl -k -f
# or
tail -f /var/log/kern.log /var/log/syslog
```

**Example denial line (what to expect):**

```
apparmor="DENIED" operation="open" profile="/usr/sbin/mydaemon" name="/etc/shadow" pid=1234 comm="mydaemon" requested_mask="r" denied_mask="r" fsuid=0 ouid=0
```

Fields explained:

* `profile`: which AppArmor profile was active
* `name`: the file path the program tried to access
* `operation`/`requested_mask`: type of access (open/read/write)
* `pid`/`comm`: process id & command

Use the log to update the profile (add allowed rules) if the access is legitimate; if the access looks malicious, deny and investigate.

---

# 7) Profiling / helper tools (speed up policy creation)

* `aa-genprof <path>` — run interactive policy generator (runs the program and records accesses, then helps create rules).
* `aa-logprof` — parse logs (from complain mode) and propose profile changes.
* `apparmor_parser` — compile/load text profiles.
* `aa-status` — lists loaded profiles + modes.

Workflow:

1. `aa-complain /usr/sbin/mydaemon`
2. Run the program and exercise all functionality.
3. `aa-logprof` to review suggested changes and update profile.
4. Switch to `aa-enforce` when satisfied.

---

# 8) Auto-start the daemon (init/systemd) on BusyBox rootfs

**BusyBox init (simple /etc/inittab):**
Add line in `rootfs/etc/inittab`:

```
::respawn:/usr/sbin/mydaemon
```

Make sure your AppArmor profile is copied to `rootfs/etc/apparmor.d/` and `apparmor` userland loads it pre-init (or embed loader into init scripts).

**systemd (if used on your distro):**

* systemd will honor AppArmor profiles that exist for the ExecStart binary path. To ensure profile is loaded before service starts, create dependency on `apparmor.service`:

```ini
[Unit]
Description=MyDaemon
After=apparmor.service
Requires=apparmor.service

[Service]
ExecStart=/usr/sbin/mydaemon
Restart=always

[Install]
WantedBy=multi-user.target
```

(Ensure AppArmor profiles are loaded during early boot — on many distros `apparmor.service` handles that.)

---

# 9) Troubleshooting & common gotchas

* **Profile path mismatch**: AppArmor matches the canonical program path at exec time. If you move/rename binary or use symlinks, the profile may not match. Use the same canonical path or use a wildcard profile (less recommended).
* **Hardlinks / path-based bypass**: Path-based profiles can be bypassed by tricks like hardlinks and execve via fds in weird cases. Avoid exposing writable directories with executables.
* **Dynamic libraries**: If binary is dynamic, ensure libraries under `/lib` are readable (add `/lib/** r, /usr/lib/** r` while developing).
* **AppArmor missing on kernel**: If `aa-status` shows AppArmor disabled, check kernel config and boot args.
* **Confusing logs**: Use `journalctl -k` or `/var/log/kern.log` and `aa-logprof` to parse messages.
* **Multiple profiles for same binary**: Only one profile applies per binary invocation — use consistent deployment.

---

# 10) Best practices for embedded devices

* Start **complain** mode during bring-up to avoid bricking device services.
* Ship profiles as part of the image (e.g., put `/etc/apparmor.d/` in your build).
* Load profiles early in boot (via `apparmor_parser` or `apparmor.service`) before services start.
* Keep profiles narrow and explicit — minimal allowed operations.
* Use automation (CI) to verify profiles after firmware changes.
* For package upgrades, ensure profile still applies to new paths/binaries.

---

# 11) Compact copy-paste script (build + test)

Below is a compact script you can run on a host (adjust paths, cross-compile as needed). It assumes a native environment with AppArmor tools installed and you are testing on the host or chrooting into `rootfs`.

```bash
#!/bin/bash
set -e
# Prepare dirs
rm -rf work
mkdir -p work/rootfs/{bin,usr/sbin,etc/apparmor.d,var/log,dev,tmp}
cd work

# 1) Write daemon
cat > mydaemon.c <<'C' 
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
int main(void){
  FILE *fp; int i=0;
  while(1){
    fp = fopen("/var/log/hello.log","a");
    if(!fp){ perror("fopen"); return 1; }
    time_t now=time(NULL);
    fprintf(fp,"mydaemon: log entry %d at %s", i++, ctime(&now));
    fclose(fp); sleep(5);
  }
  return 0;
}
C

# 2) Compile static binary
gcc -static -O2 -o mydaemon mydaemon.c

# 3) Place binary into rootfs
cp mydaemon rootfs/usr/sbin/
chmod +x rootfs/usr/sbin/mydaemon

# 4) Create simple apparmor profile
cat > rootfs/etc/apparmor.d/usr.sbin.mydaemon <<'AP'
#include <tunables/global>
/usr/sbin/mydaemon {
    /var/log/hello.log rw,
    /dev/null rw,
}
AP

# 5) (Optional) chroot to test on host: copy busybox, devices
cp /bin/busybox rootfs/bin/
sudo mknod -m 666 rootfs/dev/null c 1 3

# 6) Load profile into running system (on host)
sudo cp rootfs/etc/apparmor.d/usr.sbin.mydaemon /etc/apparmor.d/
sudo apparmor_parser -r /etc/apparmor.d/usr.sbin.mydaemon
sudo aa-complain /usr/sbin/mydaemon

# 7) Run program (on host or inside chroot)
sudo chroot rootfs /usr/sbin/mydaemon &

# 8) Watch logs
echo "Tailing kernel messages (denials will show here):"
sudo journalctl -k -f
# When satisfied, switch to enforce:
# sudo aa-enforce /usr/sbin/mydaemon
```

> Notes:
> *Running inside chroot on host may behave differently than on a separate target (AppArmor treats chrooted execs by canonical paths). For exact target testing, copy binary and profile into the target image and boot it in QEMU/board.*

---

# 12) Comparison reminder (AppArmor vs SELinux)

* AppArmor: **path-based**, easy to write/read, fast to adopt on embedded images; good for small teams.
* SELinux: **label/type enforcement**, more expressive & robust to path tricks, but heavier to set up and maintain (requires file labeling/relabelling in build/OTA flows).

---

If you want, next I can:

* Produce an **aa-genprof** walkthrough and show exact `aa-logprof` output for our daemon (automated profile generation).
* Produce the equivalent **SELinux workflow** (we already discussed it earlier) so you can directly compare the two confinement models for the same daemon.
* Generate a ready-to-flash Yocto recipe snippet to include AppArmor profiles and `apparmor-parser` invocation at boot.

Which follow-up would you like first?
