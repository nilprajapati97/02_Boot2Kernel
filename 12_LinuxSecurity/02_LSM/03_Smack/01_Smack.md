Nice — let’s build **Smack (Simplified Mandatory Access Control Kernel)** from the ground up, focused on how it works in embedded Linux and how you’d actually use it in practice. I’ll explain the model, kernel/user interfaces, labels/attributes, rule format, mounting/boot steps, examples (commands), common gotchas, and best practices — all with pointers to authoritative docs so you can follow up.

# What is Smack (high level)

Smack is a **lightweight, label-based Mandatory Access Control (MAC)** LSM whose explicit design goal is simplicity for embedded/mobile systems. It gives every *subject* (process) and *object* (file, socket, another process, etc.) a short text **label**, and access decisions are made by comparing those labels against a tiny rule table — if there’s a matching rule, access is allowed; otherwise it’s denied (there are a few small, well-defined label exceptions). Smack is intentionally much simpler than SELinux, which makes it attractive for embedded stacks that want MAC without heavy policy complexity. ([Linux Kernel Documentation][1], [LWN.net][2])

# Design goals & where it’s used

* **Simplicity**: small set of primitives (labels + simple rules) so integrators can reason about and maintain policy easily.
* **Low overhead / embedded-friendly**: works well for Tizen, AGL and other embedded stacks (historly used by Tizen/AGL, Legato, etc.). ([agl-gsod-2020-demo-mkdocs.readthedocs.io][3], [docs.legato.io][4])
* **Label equality**: Smack treats labels as opaque strings and only compares them for equality — no complex grammar or role/type systems. ([Linux Kernel Documentation][1])

# Core concepts (the mental model)

1. **Labels** — ASCII strings attached to processes and objects. They are unstructured text (case sensitive); recommended short (e.g., `app.camera`, `system`, or single chars like `_`, `*`). There are a few reserved single-character labels (e.g. `_` = *floor*, `*` = *star*, `^` = *hat*) with special semantics. ([Linux Kernel Documentation][1])
2. **Subjects & objects** — A *subject* is a running task/process; an *object* is a file, directory, socket, etc. Every subject/object has a Smack label used for decisions. ([Linux Kernel Documentation][1])
3. **Rights** — rules use simple letters: `r` `w` `x` (read/write/execute) plus extras `a` (append), `t` (transmute — see below), `l` (lock) in some configs. These are analogous to UNIX bits but applied via label pairs. ([Medium][5])
4. **Default rules / reserved labels** — Smack defines helpful defaults so it “doesn’t get in the way” of the system unless you apply rules. For example, objects labeled `_` (floor) are generally readable/executable by everyone; `*` (star) has different semantics; and processes with `^` (hat) get read/exec allowances — consult the doc for the exact rule order. ([Linux Kernel Documentation][1])

# Where labels live (implementation)

* **Files/objects**: stored as filesystem extended attributes (xattrs) in the `security.` namespace — primarily `security.SMACK64` for the file’s access label. There are several related attributes (exec label, transmute flag, socket in/out labels, mmap label). Example attributes: `security.SMACK64`, `security.SMACK64EXEC`, `security.SMACK64TRANSMUTE`, `security.SMACK64IPIN`, `security.SMACK64IPOUT`, `security.SMACK64MMAP`. ([Schaufler CA][6], [Linux Kernel Documentation][1])
* **Processes**: the active Smack label for a running task is exposed under procfs (e.g. `/proc/<pid>/attr/current` or `/proc/self/attr/current`; newer kernel APIs also expose `.../attr/smack/current`). Privileged processes (with the right capabilities) can change their own label. ([Linux Kernel Documentation][7], [Schaufler CA][6])

# Rule storage and the Smack pseudo-filesystem

Smack exposes a tiny control filesystem (smackfs) which is usually mounted under `/sys/fs/smackfs` or under `/smack` on older setups. The runtime rule table can be written via a control file (commonly `<mountpoint>/load`) and persistent configuration is typically provided in `/etc/smack/accesses` or `/etc/smack/accesses.d/*.` — the startup script or `smackload` loads those rules into the kernel on boot. Rules take the form:

```
subject_label object_label rights
# e.g.
AppA  AppA-data  rw
```

Writing rules takes effect immediately (most systems provide helper utilities like `smackload`). ([Schaufler CA][6], [LWN.net][2])

# Important semantics (defaults & special cases)

* If no explicit rule exists for a subject/object pair, access is **denied** (Smack is allow-by-explicit-rule, with the small set of system defaults described earlier). ([Linux Kernel Documentation][1])
* **File creation behavior**: when a process creates a file, that new file normally inherits the creating process’s label (if filesystem supports xattrs). If a directory has the `SMACK64TRANSMUTE` attribute and the rule that allowed the write included the `t` (transmute) permission, then newly-created objects in that directory inherit the *directory’s* label instead of the creator’s label — useful for shared dropbox-style directories. ([Linux Kernel Documentation][1])
* **Privilege**: two important capabilities:

  * `CAP_MAC_OVERRIDE` (can override access checks),
  * `CAP_MAC_ADMIN` (can change Smack labels and rules).
    Systems often limit which label(s) get the privilege (via `/smack/onlycap`) to avoid giving *all* root processes blanket power. ([Linux Kernel Documentation][1], [LWN.net][8])

# Networking with Smack

Smack supports labeling of socket endpoints and can encode labels into outgoing/incoming packets using CIPSO-like mechanisms (so remote Smack-aware hosts can make packet-label-based decisions). Socket descriptors can carry attributes `security.SMACK64IPOUT` / `security.SMACK64IPIN` for packet labeling and filtering; using these features often requires additional network support (netlabel/CIPSO) and is less commonly used on simple embedded devices. ([Schaufler CA][6], [LWN.net][2])

# Practical — how to *use* Smack (commands & quick demo)

> These are the runnable steps to try locally on a target kernel that has Smack enabled.

1. **Kernel config / boot**

   * Enable Smack in kernel: `CONFIG_SECURITY_SMACK=y` (and suitable `CONFIG_*` such as NETLABEL if needed). Many distros/Yocto layers provide a fragment. Mountpoint: kernel creates `/sys/fs/smackfs` when module present. ([kernelconfig.io][9], [Yocto Project Documentation][10])

2. **Mount smackfs (if not auto-mounted)**

```bash
sudo mkdir -p /sys/fs/smackfs
sudo mount -t smackfs smackfs /sys/fs/smackfs   # or add fstab: smackfs /sys/fs/smackfs smackfs defaults 0 0
```

(older how-tos mount on `/smack` — check your distro’s scripts). ([Kernel.org][11])

3. **Check a process label**

```bash
cat /proc/self/attr/current      # shows current process label
# or (if present)
cat /proc/self/attr/smack/current
```

A process often runs with a label set by init/system start scripts (system daemons often run with `_` = floor). ([Linux Kernel Documentation][7])

4. **Read a file’s Smack label**

```bash
# requires getfattr (attr/xattr tools)
getfattr -n security.SMACK64 /path/to/file
# or
getfattr -d /path/to/file
```

5. **Set a file’s Smack label**

```bash
# using setfattr
sudo setfattr -n security.SMACK64 -v "AppA" /opt/myapp/config

# or using attr (older style)
sudo attr -S -s SMACK64 -V "AppA" /opt/myapp/config

# or with chsmack helper (if installed)
sudo chsmack -a AppA /opt/myapp/config
```

Labels are typically limited in length (traditionally recommended ≤ 23 chars); the kernel enforces certain character rules — follow docs. ([Schaufler CA][6], [MJM Wired][12])

6. **Make a binary start with a specific label**

```bash
# set the exec attribute on the binary — when it is exec'd the process gets this label
sudo setfattr -n security.SMACK64EXEC -v 'AppA' /usr/bin/myapp
```

Now when `/usr/bin/myapp` is executed, the kernel will start that process with the `AppA` Smack label (subject to privilege rules). ([Linux Kernel Documentation][1])

7. **Add a rule (runtime)**

```bash
# write a rule line "subject object rights" to the Smack load interface
echo "AppA AppA-data rw" | sudo tee /sys/fs/smackfs/load
# or, if your system uses /smack:
# echo "AppA AppA-data rw" | sudo tee /smack/load
```

You can also create `/etc/smack/accesses` or `/etc/smack/accesses.d/*.` and run `smackload` at boot to persist rules. Rules take effect immediately. ([Schaufler CA][6], [Medium][5])

8. **Example test**

* Create labeled file:

```bash
sudo setfattr -n security.SMACK64 -v 'AppA-data' /tmp/secret.txt
```

* Set binary exec label and rule to allow writes:

```bash
sudo setfattr -n security.SMACK64EXEC -v 'AppA' /usr/local/bin/writer
echo "AppA AppA-data w" | sudo tee /sys/fs/smackfs/load
```

* Run `writer` → if rule correct it will write; if rule removed it will fail with permission denied and the kernel logs an AVC-like denial message in `dmesg`/syslog. ([Schaufler CA][6], [LWN.net][2])

# Common gotchas & tips for embedded

* **Filesystem xattr support**: Smack works best with filesystems that support extended attributes (ext4, f2fs, etc.). If the fs lacks xattrs, labels can be lost or fallbacks used (e.g., label assigned at creation and lost across reformat). Ensure `TMPFS_XATTR` (if using tmpfs) and proper mounts. ([Schaufler CA][6])
* **Labeling during image build**: In embedded images, set file labels at image-build time (Yocto/Buildroot recipes) or run a relabel step at first boot. Otherwise required labels may be missing and services will break once enforcement is enabled. ([Yocto Project Documentation][10])
* **Privilege minimization**: Use `onlycap` techniques (and `/smack/onlycap`) to restrict which label(s) get CAP\_MAC\_ADMIN/CAP\_MAC\_OVERRIDE — don’t give all root processes full MAC power. Legato and other frameworks use a single `admin` label to confine label/rule changes to the supervisor. ([docs.legato.io][13])
* **Transmute directory pattern**: Use `SMACK64TRANSMUTE` on directories + `t` permission in rules to make created files inherit directory label — useful for shared-drop directories without loosening whole-system rules. ([Linux Kernel Documentation][1])
* **Network quirks**: Some networks reject IP options used by CIPSO; if you enable Smack packet labeling in multi-host systems, carefully test network equipment compatibility. ([LWN.net][2])

# Smack vs SELinux vs AppArmor — short comparison for embedded

* **Smack**: tiny learning curve, label-based equality model, easy to reason about, common in embedded/mobile stacks (Tizen/AGL/Legato). Good when you want simple, explicit label-to-label policies. ([Wikipedia][14], [agl-gsod-2020-demo-mkdocs.readthedocs.io][3])
* **SELinux**: very expressive (TE, roles, MLS), stronger and more flexible for complex policy, but requires more tooling, file labelling management, and maintenance. Used in Android. ([Linux Kernel Documentation][1])
* **AppArmor**: path/profile-based, easy to author, lower maintenance than SELinux but subject to path-based bypasses and less portable semantics. Good for appliance-like embedded systems (Ubuntu Core, snaps). ([Linux Kernel Documentation][1])

# When to pick Smack (guidance)

* You control the entire device image and filesystem layout.
* You want minimal policy surface and straightforward labels/rules.
* You need a solution that’s easy to reason about for app sandboxing (each app gets a unique label and limited set of rules).
* You want features like `transmute` for shared drop areas without complicated policy. ([agl-gsod-2020-demo-mkdocs.readthedocs.io][3], [Linux Kernel Documentation][1])

# Further reading (authoritative docs)

* Kernel Smack documentation — admin guide (how Smack integrates with LSM). ([Linux Kernel Documentation][1])
* Smack whitepaper (design + examples). ([Schaufler CA][6])
* LWN: “Smack for simplified access control” (good background & examples). ([LWN.net][2])
* AGL / Tizen integration notes and how embedded stacks apply Smack. ([agl-gsod-2020-demo-mkdocs.readthedocs.io][3])

---

If you want, I’ll do one of these next (pick one and I’ll do it immediately):

1. Create a **copy-paste demo**: minimal writer/reader C programs + exact Smack commands (mount smackfs, set exec label, set file label, add rule) that you can run on your QEMU/board.
2. Generate a **Yocto recipe snippet** showing how to apply labels at image build time and add startup `smackload` so your image comes pre-labeled.
3. Produce a **compact cheat-sheet** (commands and locations: which files, which xattr names, which mountpoints) you can keep in your repo.

Which one do you want now?

[1]: https://docs.kernel.org/admin-guide/LSM/Smack.html?utm_source=chatgpt.com "Smack - The Linux Kernel documentation"
[2]: https://lwn.net/Articles/244531/?utm_source=chatgpt.com "Smack for simplified access control - LWN.net"
[3]: https://agl-gsod-2020-demo-mkdocs.readthedocs.io/en/latest/flounder/architecture/reference/security/part-5/1-MAC/?utm_source=chatgpt.com "Mandatory Access Control - AGL Demo - Read the Docs"
[4]: https://docs.legato.io/18_01/howToSecuritySmack.html?utm_source=chatgpt.com "Implementing SMACK - Legato Docs"
[5]: https://mihail-milev.medium.com/using-smack-to-secure-k8s-containers-and-nodes-a-proof-of-concept-6f6cf8550c1f?utm_source=chatgpt.com "Using SMACK to secure K8s containers and nodes - Mihail Milev"
[6]: https://schaufler-ca.com/assets/docs/SmackWhitePaper.257153003.pdf?utm_source=chatgpt.com "[PDF] The Simplified Mandatory Access Control Kernel - The Smack Project"
[7]: https://docs.kernel.org/userspace-api/lsm.html?utm_source=chatgpt.com "Linux Security Modules - The Linux Kernel documentation"
[8]: https://lwn.net/Articles/292128/?utm_source=chatgpt.com "smack: limit privilege by label - LWN.net"
[9]: https://www.kernelconfig.io/config_security_smack?utm_source=chatgpt.com "config_security_smack - kernelconfig.io"
[10]: https://docs.yoctoproject.org/pipermail/yocto/2016-October/032709.html?utm_source=chatgpt.com "[yocto] [meta-security][PATCH 2/2] smack kernel"
[11]: https://www.kernel.org/doc/Documentation/security/Smack.txt?utm_source=chatgpt.com "Smack.txt - The Linux Kernel Archives"
[12]: https://mjmwired.net/kernel/Documentation/security/Smack.txt?utm_source=chatgpt.com "Linux Kernel Documentation :: security : Smack.txt"
[13]: https://docs.legato.io/16_01/howToSecuritySmack.html?utm_source=chatgpt.com "Implementing SMACK - Legato Docs"
[14]: https://en.wikipedia.org/wiki/Smack_%28software%29?utm_source=chatgpt.com "Smack (software)"
