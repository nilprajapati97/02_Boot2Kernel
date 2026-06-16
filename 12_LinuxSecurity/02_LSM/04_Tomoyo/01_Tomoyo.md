# TOMOYO Linux — in-depth, from scratch (embedded-focused)

Nice — let’s build TOMOYO from the ground up, block-by-block, so you understand the model, the kernel/user interfaces, the policy lifecycle (learning → refine → enforce), practical commands, embedded integration tips, and limitations. I’ll highlight the exact files/tools you’ll use and give copy-ready snippets you can drop into a Yocto/BusyBox flow.

---

## TL;DR (one line)

TOMOYO is a **pathname/name-based Mandatory Access Control (MAC) LSM** that learns and records program behaviour (domains) and produces very human-readable, path-oriented policies you can refine and then enforce — making it especially handy for embedded bring-up and system analysis. ([Linux Kernel Documentation][1], [ArchWiki][2])

---

## High-level design goals

* **Behavior-first**: model each *process* by the executable pathname it runs and record the resources it touches (files, network, IPC) so you can later restrict that behavior.
* **Human-friendly policy**: policies are textual, readable, and organized by *domain* (which maps to an executable path) — easier for developers than SELinux TE.
* **Learning mode**: capture normal behavior in a permissive/learning state, then convert logs to policy — ideal during board bring-up.
* **Small footprint**: few dependencies, simple userspace tools, suitable for embedded devices. ([TOMOYO Linux][3], [ArchWiki][2])

---

## Block-by-block architecture

### 1) LSM integration & pathname helpers (kernel block)

* TOMOYO is an LSM built into the kernel (`CONFIG_SECURITY_TOMOYO`) and uses pathname helpers so checks are based on canonical path names rather than labels. The kernel exposes the TOMOYO control interface under `securityfs` (`/sys/kernel/security/tomoyo/`), which userspace reads/writes to load policy and view audit logs. ([Cateee][4], [TOMOYO Linux][5])

### 2) Domain generation (process identity)

* **Domain = an execution chain / canonical pathname key**. When a program execs, the kernel records the execution pathname and associates that process with a domain name (usually the pathname itself or a short alias). During learning, new domains are automatically discovered and recorded for later policy creation. ([TOMOYO Linux][6])

### 3) Policy engine & sysfs control (enforcement block)

* The policy is represented as a set of structured files/objects that map domains → allowed actions (file read/write/create, execute, network accept/connect, capabilities, IPC, mounts, etc.). At runtime policy is loaded into kernel memory via `/sys/kernel/security/tomoyo/*` interfaces and enforced in the kernel LSM hooks. ([TOMOYO Linux][5])

### 4) Learning engine & logging (capture block)

* When a domain is in **learning mode**, TOMOYO logs *granted* and *rejected* accesses (so you can see what you should allow). Tools then convert these logs into candidate policy lines. This is the core UX: run the system in learning mode during functional bring-up, exercise features, then convert logs to a policy. ([TOMOYO Linux][3])

### 5) Policy components (storage / format block)

* The TOMOYO policy is typically stored under `/etc/tomoyo/policy/...` and consists of several files: **domain\_policy.conf**, **exception\_policy.conf**, **profile.conf**, **manager.conf**, etc. `tomoyo-savepolicy` writes these from kernel memory; `tomoyo-init` / `tomoyo-loadpolicy` load them on boot. These files are readable and editable text. ([Ubuntu Manpages][7])

### 6) User tools & UI (userspace block)

* `tomoyo-init`, `tomoyo-loadpolicy`, `tomoyo-savepolicy`, `tomoyo-editpolicy` (interactive), `tomoyo-pstree` (process tree viewer), `tomoyo-queryd` and `tomoyo-notifyd` are common tooling around TOMOYO. They let you initialize policy templates, interactively edit domain rules, accept/reject runtime access requests, and persist the resulting policy. ([carta.tech][8], [Debian Manpages][9])

### 7) Profiles & modes (policy modes block)

* TOMOYO defines **profiles** (profile 0 = Disabled, 1 = Learning, 2 = Permissive, 3 = Enforcing) and you can select which profile applies to each domain or group. You can also control categories (file, network, exec, capability) independently, e.g., put file ops in learning but execution in enforcing while building the rest. This fine-grained profile control is very practical for staged lockdown. ([LWN.net][10])

---

## Policy model & concrete syntax (what you actually edit)

* Policy is **domain-centric**. A domain entry looks like:

```
<kernel> /usr/sbin/httpd
  file read /var/www/html/*.html
  network inet stream accept 0.0.0.0 80
```

* The **exception policy** holds permissions applied globally (shortcuts/groupings). You can define `path_group` and `number_group` to make policies compact (`@WEB-CONTENTS`, `@WEB-CLIENT-PORTS`). Tools like `tomoyo-patternize` can convert concrete paths into patterns. Example pattern and domain usage shown in TOMOYO docs. ([TOMOYO Linux][11])

---

## Typical workflow (embedded bring-up → production)

1. **Kernel & image:** enable `CONFIG_SECURITY_TOMOYO` (and `SECURITYFS`/`SECURITY_PATH` as needed) in your kernel and include tomoyo-tools in the image. You can also add `security=tomoyo` to kernel cmdline to make init run the tomoyo init hook. ([Android Git Repositories][12], [docs.slackware.com][13])
2. **Init policy templates:** run `tomoyo-init` (or ship pre-filled `/etc/tomoyo/init_policy`) so the system has the policy files layout. ([Ubuntu Manpages][14])
3. **Reboot & generate domains:** after reboot TOMOYO will log execution chains and populate domain lists.
4. **Switch target domains to Learning:** use `tomoyo-editpolicy` to set broad domains (or `<kernel>` namespace) into **learning** and exercise all device functions (boot scripts, daemons, apps). TOMOYO will record the accesses. ([ArchWiki][2])
5. **Review & convert logs:** use the interactive editor or `tomoyo-savepolicy` to save logs to `/etc/tomoyo/policy/...` and refine (patternize, create groups). ([Ubuntu Manpages][7], [TOMOYO Linux][11])
6. **Harden:** switch domains from learning → permissive → enforcing one by one, testing each change. TOMOYO lets you do this per-domain and per-category. ([TOMOYO Linux][15])

---

## Practical commands / examples

Initialise policy files (first boot / in build):

```bash
# on target or within image build final stage
/usr/sbin/tomoyo-init    # generates /etc/tomoyo templates and (on boot) loads policy
```

Load a saved profile manually:

```bash
tomoyo-loadpolicy -p < /etc/tomoyo/profile.conf
tomoyo-loadpolicy -d < /etc/tomoyo/domain_policy.conf
```

Save running policy to disk:

```bash
tomoyo-savepolicy
# creates /etc/tomoyo/policy/YY-MM-DD.hh:mm:ss with domain_policy.conf etc.
```

Interactive editing / switching domains:

```bash
tomoyo-editpolicy   # curses UI: select domain, set profile (learning/enforce), edit rules
```

View TOMOYO sysfs interface:

```bash
ls /sys/kernel/security/tomoyo
# contains: audit, domain_policy, exception_policy, manager, profile, query, self_domain, stat, version
```

(You can write policy into those sysfs nodes when using low-level tools; tomoyo-init/loadpolicy do this for you). ([TOMOYO Linux][5], [Debian Manpages][9])

---

## Embedded integration tips (practical, from experience)

* **Ship initial templates**: generate `/etc/tomoyo/policy/current/*` on your build host (Yocto recipe) and include them in the image so first boot is predictable. `tomoyo-init` can be invoked automatically by init if `security=tomoyo` is set. ([Ubuntu Manpages][14])
* **Learning window**: during manufacturing/bring-up, run a test suite with the system set to learning for relevant domains — the richer the exercise, the more complete the resulting policy.
* **Automate patternization**: use `tomoyo-patternize` or the tools’ pattern functions to collapse noisy temp path names into groups (e.g., `/var/tmp/…` patterns). ([TOMOYO Linux][11])
* **Persist only vetted policies**: `tomoyo-savepolicy` saves snapshots; review and possibly hand-tune before shipping to production. ([Ubuntu Manpages][7])
* **Early boot ordering**: ensure TOMOYO policy is loaded before services you want to confine. Use an init script or systemd unit that runs early and depends for services on tomoyo.service if your distro provides one. ([docs.slackware.com][13])

---

## Strengths, limitations & comparison

Strengths

* Very **easy to read** and author (pathname + directive text). Great for teams that want quick, comprehensible policies.
* **Learning mode** is arguably TOMOYO’s killer feature for embedded: generate policy from observed runtime behavior. ([TOMOYO Linux][3])

Limitations / gotchas

* **Pathname based**: like AppArmor, TOMOYO decisions use file pathnames, which can be sensitive to symlink/hardlink/`execve`-by-fd races — kernel pathname helpers mitigate many issues but this is a fundamental tradeoff vs label-based systems (SELinux/Smack). Be careful whenever you accept untrusted file uploads or allow writable directories with executables. ([Huihoo][16], [ArchWiki][2])
* **Requires learning effort**: although learning mode helps, producing minimal least-privilege policies takes time and careful test coverage. The docs recommend rebuilding policy from scratch via learning if the service behavior changes significantly. ([TOMOYO Linux][15])
* **Less ecosystem in some spaces**: Android and many automotive stacks standardized on SELinux/Smack; TOMOYO is excellent for small appliances and servers but less common in those ecosystems. ([Linux Kernel Documentation][1])

---

## When to pick TOMOYO for embedded

* You want straightforward, readable policy editability and a strong “learning” workflow for bring-up.
* Your product team prefers quick iteration and human-readable rules vs the heavier tooling and label management of SELinux.
* You control the entire image and can ensure policy is loaded at boot and tested in CI/QA. ([ArchWiki][2])

---

## Want a hands-on demo?

I can immediately produce one of these (pick one — I’ll create it right here):

1. A **copy-paste demo**: tiny BusyBox rootfs + a simple service, exact `tomoyo-init`, `tomoyo-editpolicy` steps, and sample `domain_policy.conf` snippets you can run in QEMU.
2. A **Yocto recipe snippet** that: enables `CONFIG_SECURITY_TOMOYO`, installs `tomoyo-tools`, runs `tomoyo-init` at first boot and includes a policy snapshot.
3. An **interactive learning walkthrough** showing the exact sequence: set domain to learning, exercise a scenario, run `tomoyo-savepolicy`, patternize, switch to enforcing — with all exact commands and sample outputs.

Which would you like me to build now?

[1]: https://docs.kernel.org/admin-guide/LSM/tomoyo.html?utm_source=chatgpt.com "TOMOYO - The Linux Kernel documentation"
[2]: https://wiki.archlinux.org/title/TOMOYO_Linux?utm_source=chatgpt.com "TOMOYO Linux - ArchWiki"
[3]: https://tomoyo.sourceforge.net/1.8/chapter-4.html.en?utm_source=chatgpt.com "TOMOYO Linux 1.8.x : The Official Guide : Chapter 4"
[4]: https://cateee.net/lkddb/web-lkddb/SECURITY_TOMOYO.html?utm_source=chatgpt.com "CONFIG_SECURITY_TOMOYO: TOMOYO Linux Support"
[5]: https://tomoyo.sourceforge.net/2.6/policy-specification/index.html?utm_source=chatgpt.com "TOMOYO Linux 2.6.x : Policy specification"
[6]: https://tomoyo.sourceforge.net/1.8/chapter-5.html.en?utm_source=chatgpt.com "Chapter 5: How do I manage domains? - TOMOYO Linux"
[7]: https://manpages.ubuntu.com/manpages/xenial/man8/tomoyo-savepolicy.8.html?utm_source=chatgpt.com "tomoyo-savepolicy - save TOMOYO Linux policy - Ubuntu Manpage"
[8]: https://carta.tech/packages/tomoyo-tools/?utm_source=chatgpt.com "Man Pages in tomoyo-tools - Carta.tech"
[9]: https://manpages.debian.org/unstable/tomoyo-tools/tomoyo-init.8?utm_source=chatgpt.com "tomoyo-init(8) — tomoyo-tools — Debian unstable - Debian Manpages"
[10]: https://lwn.net/Articles/351024/?utm_source=chatgpt.com "TOMOYO Linux version 1.7.0 released. - LWN.net"
[11]: https://tomoyo.sourceforge.net/1.8/chapter-6.html.en?utm_source=chatgpt.com "TOMOYO Linux 1.8.x : The Official Guide : Chapter 6"
[12]: https://android.googlesource.com/kernel/msm/%2B/android-6.0.1_r0.84/security/tomoyo/Kconfig?utm_source=chatgpt.com "security/tomoyo/Kconfig - kernel/msm - Git at Google"
[13]: https://docs.slackware.com/howtos%3Asecurity%3Atomoy_linux_basics_slackware?do=export_pdf&utm_source=chatgpt.com "[PDF] Mandatory Access Control - Getting started with Tomoyo Linux on ..."
[14]: https://manpages.ubuntu.com/manpages/focal/man8/tomoyo_init_policy.8.html?utm_source=chatgpt.com "init_policy - initialize TOMOYO Linux policy - Ubuntu Manpage"
[15]: https://tomoyo.sourceforge.net/1.8/chapter-7.html.en?utm_source=chatgpt.com "Chapter 7: How do I enforce policy? - TOMOYO Linux"
[16]: https://docs.huihoo.com/doxygen/linux/kernel/3.7/security_2tomoyo_2common_8h.html?utm_source=chatgpt.com "Linux Kernel: security/tomoyo/common.h File Reference - Huihoo"
