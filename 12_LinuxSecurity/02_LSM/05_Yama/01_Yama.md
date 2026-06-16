# Yama — deep dive **from scratch** (Linux kernel security, ptrace hardening)

Short summary up front: **Yama** is a small Linux Security Module whose main job is to tighten and centralize discretionary-access (DAC) protections around the `ptrace()` interfaces so that a compromised user-level program can’t trivially attach to and read/modify other processes of the same user. It’s tiny, widely used for hardening, and controlled at runtime by `/proc/sys/kernel/yama/ptrace_scope`. ([Kernel.org][1], [LKML][2])

---

## 1) Why Yama exists (threat model)

* `ptrace()` is a powerful debugging syscall: it lets one process read/write another process’s memory, registers, control its execution, etc. If any ordinary process could `PTRACE_ATTACH` any other process owned by the same UID, a single compromise (e.g., of a chat app) could be used to harvest secrets from browsers, SSH agents, wallets, etc.
* Yama provides system-wide policy to **limit which processes can attach** to which other processes — reducing lateral theft via tracing. This is a pragmatic, small-surface containment measure for everyday hardening. ([Kernel.org][1])

---

## 2) Where Yama lives (build / runtime)

* Yama is selectable at kernel build-time with `CONFIG_SECURITY_YAMA`. When present it exposes runtime knobs under `/proc/sys/kernel/yama/` — primarily `ptrace_scope`. The sysctl is writable only by a process with `CAP_SYS_PTRACE`. ([Kernel.org][1])

---

## 3) The `ptrace_scope` modes — exact semantics (the core)

`/proc/sys/kernel/yama/ptrace_scope` controls behavior. The kernel docs spell out the modes — **memorize these** because they determine what debugging and tracing will work:

* `0` — **classic/unrestricted**: legacy behavior. A process can `PTRACE_ATTACH` to any other process with the same UID **if** that target is “dumpable” (i.e., it didn’t drop dumpability by UID transition, didn’t start privileged, and hasn’t called `prctl(PR_SET_DUMPABLE,0)`). `PTRACE_TRACEME` is unchanged.
* `1` — **restricted (default on many distros)**: only processes that have a **predefined relationship** with the target may `PTRACE_ATTACH` (by default that relationship is “descendant” — parent/children). An inferior (the target) can call `prctl(PR_SET_PTRACER, pid)` to allow a specific debugger PID to attach (used by crash handlers, Wine, Chromium, etc.). `PTRACE_TRACEME` is unchanged.
* `2` — **admin-only attach**: only processes with `CAP_SYS_PTRACE` may use `PTRACE_ATTACH` (or have children `PTRACE_TRACEME`).
* `3` — **no attach**: no one may attach (neither `PTRACE_ATTACH` nor `PTRACE_TRACEME`); **once set to 3 it cannot be changed** until a reboot.
  These are taken verbatim from kernel docs — they’re the authoritative behavior. ([Kernel.org][1])

---

## 4) Interaction with `prctl()` and `PR_SET_DUMPABLE` / `PR_SET_PTRACER`

* `PR_SET_DUMPABLE` influences whether a process is “attachable” under classic rules — many programs (ssh-agent, setuid helpers) set non-dumpable to reduce attachments. `PR_SET_PTRACER` is the mechanism a process (the *inferior*) can call to *whitelist a specific debugger* (by PID) to attach when Yama is restricting ptrace. There’s also a `PR_SET_PTRACER_ANY` option that makes the process allow any otherwise-allowed debugger (be careful, that widens access). The kernel docs explain these primitives and recommend use for legitimate debugger/crash-handler relationships. ([Kernel.org][1])

Practical C snippet (allowing any debugger — use with caution):

```c
#include <sys/prctl.h>
prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
```

Or to allow a particular debugger PID:

```c
prctl(PR_SET_PTRACER, (unsigned long)debugger_pid, 0, 0, 0);
```

(These are standard `prctl` calls; check `<sys/prctl.h>` on your target.)

---

## 5) Who can change the setting

* Only a process with `CAP_SYS_PTRACE` can write `/proc/sys/kernel/yama/ptrace_scope`. Typical usage: root or a process with the capability. Many distributions expose the knob to admins but keep it restricted by default. ([Kernel.org][1])

---

## 6) Practical commands (check/change)

```bash
# view current mode
cat /proc/sys/kernel/yama/ptrace_scope
# or
sysctl kernel.yama.ptrace_scope

# temporarily set (root)
echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope

# permanent: add to sysctl.d (example to persist)
# create /etc/sysctl.d/10-ptrace.conf with:
# kernel.yama.ptrace_scope = 1
# then apply with:
sudo sysctl --system
```

Note: `/proc` lives in RAM, so echoing a value only lasts until reboot — persist via `/etc/sysctl.d/*.conf` or other distro mechanism. ([Unix & Linux Stack Exchange][3], [Kernel.org][1])

---

## 7) Typical distro defaults & why (operational guidance)

* Many distributions enable **`ptrace_scope = 1`** (restricted) by default as a security hardening: it prevents cross-process memory reads among sibling processes of the same UID while still allowing normal parent/child debugging and crash-handler arrangements. Hardening scanners / CIS checks recommend restricting ptrace usage. If you need full debugging across arbitrary processes you can temporarily set the knob to `0` or grant `CAP_SYS_PTRACE` to the debugging binary. ([Linux Audit][4], [Tenable®][5])

---

## 8) How to allow a specific debugger without opening the whole system

Options (prefer to least privilege):

1. **Use `prctl(PR_SET_PTRACER, pid)` from the inferior** — the target process can explicitly allow a given debugger PID to attach when scope=1. This is the crash-handler model used by Chromium, KDE, Wine. ([Kernel.org][1])
2. **Grant the debugger `CAP_SYS_PTRACE`** (via `setcap cap_sys_ptrace=ep /path/to/gdb`) — then it can attach under scope=2. This is useful for CI/test machines where you want certain tools to have tracing power. ([Ask Ubuntu][6])
3. **Temporarily set `ptrace_scope=0`** while debugging, then restore to the hardened value.

---

## 9) Production recommendations & tradeoffs

* **Default for many production deployments:** `ptrace_scope = 1` (restricted) — good balance between enabling legitimate debuggers and preventing lateral data theft from a compromised user process. For highly conservative deployments with no need for user-side debugging, `2` (admin-only) or `3` (no attach) can be used — but remember `3` is irreversible until reboot. ([Kernel.org][1], [Tenable®][5])
* **Developer machines / CI:** `0` is convenient for full debugging. If you need specific tools to attach in hardened environments, prefer giving those tools `CAP_SYS_PTRACE` rather than globally relaxing `ptrace_scope`. ([Ask Ubuntu][6])

---

## 10) Troubleshooting (common messages and fixes)

* Error when attaching with gdb/llbd: `ptrace: Operation not permitted` — likely Yama blocking. Quick checks:

  ```bash
  cat /proc/sys/kernel/yama/ptrace_scope
  # if 1 or 2, either change to 0 temporarily (echo 0 > ...) or grant cap/set PR_SET_PTRACER as appropriate
  ```

  Many tools and project docs (LLDB, Wine, Open-TEE) mention this as the common root cause and advise either setting `ptrace_scope=0` temporarily or using `setcap`/`prctl`. ([reviews.llvm.org][7], [open-tee.github.io][8])

---

## 11) History & adoption (brief)

* Yama’s code and rationale were merged into the kernel tree around the 3.4 cycle (initially submitted by Kees Cook / collaborators) to provide a compact place for DAC-related hardening rules such as ptrace restrictions; Chrome OS and several distros adopted it early. (Useful context if you’re reading older docs or backports.) ([LKML][2], [outflux.net][9])

---

## 12) Quick cheat-sheet (copy/paste)

```bash
# show current
cat /proc/sys/kernel/yama/ptrace_scope

# temporarily allow classic (debugging)
sudo sh -c 'echo 0 > /proc/sys/kernel/yama/ptrace_scope'

# permanently set restricted
echo "kernel.yama.ptrace_scope = 1" | sudo tee /etc/sysctl.d/10-ptrace.conf
sudo sysctl --system

# grant ptrace capability to a debugger (example)
sudo setcap cap_sys_ptrace+ep /usr/bin/gdb
```

Remember: writing `/proc/sys/kernel/yama/ptrace_scope` requires `CAP_SYS_PTRACE` (normally root). ([Kernel.org][1], [Unix & Linux Stack Exchange][3])

---

## 13) Final notes — how to think about Yama in your product

* Treat Yama as **low-friction, high-payoff hardening**: it fixes a real attack surface (cross-process tracing) with a tiny kernel feature.
* For embedded/production devices, set the strictest mode compatible with your operational needs (1 or 2), ship a tested mechanism to relax for debugging (e.g., a dev/diagnostic image or a controlled capability grant), and prefer per-process consent (`PR_SET_PTRACER`) or capability grants over global relaxation. ([Kernel.org][1], [Tenable®][5])

---

If you want, I can now:

* produce a tiny demo (small C programs showing `PR_SET_PTRACER` and a simple tracer) you can run on QEMU, **or**
* give a short systemd/Yocto recipe snippet that sets `ptrace_scope` persistently for your embedded image, **or**
* audit typical dev/debug flows and propose safe procedures (CI, OTA, production) for using Yama in your product.

Which would you like next?

[1]: https://www.kernel.org/doc/Documentation/security/Yama.txt "www.kernel.org"
[2]: https://lkml.org/lkml/2012/3/20/510?utm_source=chatgpt.com "James Morris: [GIT] Security subsystem updates for 3.4 - LKML"
[3]: https://unix.stackexchange.com/questions/329504/proc-sys-kernel-yama-ptrace-scope-keeps-resetting-to-1?utm_source=chatgpt.com "proc/sys/kernel/yama/ptrace_scope keeps resetting to 1"
[4]: https://linux-audit.com/protect-ptrace-processes-kernel-yama-ptrace_scope/?utm_source=chatgpt.com "Protect against ptrace of processes: kernel.yama.ptrace_scope"
[5]: https://www.tenable.com/audits/items/CIS_Debian_Linux_12_v1.1.0_L1_Server.audit%3A955ba0536d379e26448b4251f9736f39?utm_source=chatgpt.com "1.5.2 Ensure ptrace_scope is restricted - Tenable"
[6]: https://askubuntu.com/questions/146160/what-is-the-ptrace-scope-workaround-for-wine-programs-and-are-there-any-risks?utm_source=chatgpt.com "What is the 'ptrace_scope' workaround for Wine programs and are ..."
[7]: https://reviews.llvm.org/D144904?utm_source=chatgpt.com "[Linux] Add kernel.yama.ptrace_scope note when ptrace fails to ..."
[8]: https://open-tee.github.io/faq/?utm_source=chatgpt.com "FAQ - Open-TEE"
[9]: https://outflux.net/slides/2012/lss/lsm/Yama%20%282012%20Progress%20Report%29.pdf?utm_source=chatgpt.com "[PDF] Security Subsystem Report: Yama - outflux.net"
