# TOMOYO LSM — in-depth, block-wise (from scratch)

Nice — TOMOYO is a lightweight, **path-based** Linux Security Module (LSM) focused on *simplicity, learnability and easy policy creation*, which makes it attractive for embedded systems. Below I’ll explain TOMOYO block-by-block: goals, architecture, policy model, enforcement flow, lifecycle (learn → refine → enforce), internals/hook points, pros/cons, comparisons, deployment steps and a practical example (conceptual policy + deployment flow). Diagrams are ASCII so you can copy them into notes.

---

# 1) High level — What TOMOYO is and why it exists

* **What:** TOMOYO is an LSM that enforces Mandatory Access Control using **executable path names** (rather than labels). It intercepts security-relevant kernel operations and consults a policy expressed around programs (domains), files by path, network, capabilities and process transitions.
* **Why:** simplicity and low operational overhead. TOMOYO’s path model is easy to understand and to author by observing actual system behavior (learning mode). That makes it well suited to embedded/Appliance use where you want a small trusted policy footprint and few administrators.

---

# 2) Core design blocks (big picture)

```
[ Process (exec / syscalls) ]
         |
         v
   LSM hook points (TOMOYO)
         |
         v
   TOMOYO policy engine (in kernel + policy DB)
         |
    Decision: Allow / Deny / Log
         |
         v
   (optionally) audit to userspace / learning store
```

Breakdown into blocks:

1. **Domain creation/identification** – process is assigned a *domain* (policy context) based on executable path and how it was invoked.
2. **Policy database** — rules grouped by domain: file access, directory access, network, capabilities, exec transitions, mount operations, etc.
3. **Hook & enforcement engine** — kernel LSM hooks call TOMOYO checks at syscall points and decide.
4. **Userspace tools & learning** — utilities (userspace companion) record observed accesses in *learning mode* and help generate policies.
5. **Policy lifecycle** — learning → edit/validate → enforce.

---

# 3) Domain model & identification (path-based)

* **Domain = running program / domain for process**. TOMOYO typically uses the *absolute path of the executable* to identify the domain. Example domain identifiers: `/sbin/init`, `/usr/bin/ssh`, `/usr/lib/hal/camera`.
* **Transition rules:** when a process `A` executes `B`, TOMOYO can record a *transition* rule (A → B) and create a new domain for B (or permit transition). This allows controlled escalation of privileges only via known transitions.
* **Why path-based:** easier to reason — administrators think “this binary can do X” rather than dealing with object labels and type enforcement.

---

# 4) Policy primitives (what you can express)

TOMOYO policies are typically grouped under domains. Common rule types:

* **File access rules**

  * allow read/write/execute/create/unlink on specific paths or directories (prefix matching).
* **Directory rules**

  * allow access to all files under `/var/log/myapp/` etc.
* **Network rules**

  * allow bind/connect/listen for IPv4/IPv6 sockets and protocol/port restrictions.
* **Capability rules**

  * permit/deny Linux capabilities like `CAP_NET_ADMIN`, `CAP_SYS_ADMIN`.
* **Mount / remount rules**

  * control mounting of filesystems.
* **Process exec/transition**

  * allow domain to `exec` another program (transition) and create the child domain rule.
* **Signal / ptrace / process control**

  * restrict sending signals or ptrace operations between domains.
* **Syscall level**

  * some fine-grained controls on certain operations.

> TOMOYO policies are generally **path-aware** (prefix, wildcard) and human-readable.

---

# 5) Enforcement flow — step-by-step

1. **Process issues syscall / attempts exec / opens file / binds socket**
2. **Kernel LSM hook triggers** (e.g., `security_file_open`, `security_socket_create`, `security_bprm_check` for exec)
3. **TOMOYO policy engine** checks:

   * Which domain does the calling process belong to? (path of its executable; also consider transition history)
   * Which rule type is relevant for the attempted operation?
   * Is there a matching allow rule? If yes → allow. If no → deny (or log/notify depending on mode).
4. **Action**:

   * **Enforce mode**: deny (return -EACCES / -EPERM) and log AVC-like message.
   * **Learning mode**: allow operation but record it into a learning store for later policy generation.
5. **Userspace notification** (optional): tools pick up logs / learning info so admin can accept/merge into policy.

ASCII flow:

```
[syscall] -> LSM-hook -> identify domain -> lookup rule set
           -> rule found? yes -> allow (return to syscall)
                       no  -> learning? record & allow : deny + log
```

---

# 6) Policy lifecycle: Learn → Refine → Enforce (typical embedded flow)

1. **Boot in learning mode** (TOMOYO starts permissive but logs everything).

   * Run typical device workloads and user journeys.
   * The system records what file accesses, execs and network operations occur per domain.
2. **Review generated policy** in userspace tools.

   * Remove noisy/undesired rules, tighten directories to exact paths, drop broad wildcards.
3. **Switch to enforcing mode** and test.

   * Observe denies and iterate (you may temporarily add allow rules or fix code).
4. **Harden**: remove dev aids, restrict capabilities, remove unnecessary transitions.

This “train then lock” workflow is one of TOMOYO’s strongest selling points for embedded devices.

---

# 7) Userspace tooling & storage (where policies live)

* **Policy store**: TOMOYO policies are commonly stored in human-readable files (often under `/etc/tomoyo/` or similar). The kernel can load policy at boot.
* **Learning/log**: tomoyo’s userspace tools (on some distros `tomoyo-tools`) gather access traces and help convert them to policy entries.
* **Edit/merge**: admins review/adjust before activating enforcement.
* **On embedded systems**: you often store the final policy in the readonly rootfs and mount a tiny read/write area for volatile stuff.

> I’m intentionally not listing exact userspace binary names here because distributions vary; the important part is the flow: learning data → conversion → policy file → load.

---

# 8) Example (conceptual) — policy snippet (pseudo-TOMOYO)

> This is conceptual pseudo-syntax to show structure (actual command/syntax varies by TOMOYO userspace). Treat it as readable policy, not exact file format.

```
# Domain: /usr/bin/mydaemon
domain /usr/bin/mydaemon {
    # File permissions (path prefix)
    allow_read  /etc/mydaemon/config.yaml
    allow_write /var/lib/mydaemon/*.db
    allow_write /var/log/mydaemon/*.log

    # Directory-level access
    allow_read  /usr/share/mydaemon/*

    # Exec transitions (what this daemon may exec)
    allow_exec  /usr/bin/helper_tool
    allow_exec  /bin/sh   # maybe for controlled scripts (avoid if possible)

    # Network rules (allow only outbound TCP to specific host:port)
    allow_net_connect  tcp 192.0.2.10:443

    # Capabilities
    allow_capability  CAP_NET_BIND_SERVICE   # if needs to bind to low ports
}
```

**Behavior**: a process running `/usr/bin/mydaemon` will only be allowed the actions above. Any other file/network/capability attempts will be blocked (if enforcing) or recorded (if learning).

---

# 9) Advantages (why pick TOMOYO)

* **Simplicity & readability** — policy is path-based and often easier for human operators to reason about than SELinux TE rules.
* **Learning mode** — robust tooling to generate policies by observing real behavior; great for appliance/device builders.
* **Lightweight** — lower concept load; easier onboarding for embedded engineers who are not SELinux experts.
* **Fine-grained path control** — you can write allowlists that precisely limit filesystem accesses and transitions.

---

# 10) Limitations and trade-offs

* **Path-based model is weaker against name/replace attacks**

  * If an attacker can replace executable at path `/usr/bin/foo` with a malicious binary, they inherit that domain’s privileges. Mitigation: use immutable/read-only rootfs, signed updates and dm-verity.
* **Less formal than SELinux**

  * SELinux’s label/type model is more flexible for multi-tenant, label-based policies and for integrating with other systems. TOMOYO trades some formalism for simplicity.
* **Fewer distributions / ecosystem tools** vs SELinux/AppArmor (SELinux and AppArmor are more widely packaged/used in some distros).
* **Policy scale** — for very large, dynamic systems (many versions of binaries or heavy container churn) TOMOYO policy maintenance can become work; it shines in stable embedded images.

---

# 11) TOMOYO vs SELinux vs AppArmor (quick comparison)

* **SELinux**: label/type-based, very expressive, steep learning curve, strong for multi-tenant or high-assurance systems. Good for Android and server distros.
* **AppArmor**: path-based like TOMOYO but profile syntax and tooling differ; integrated into Ubuntu/OpenSUSE; good compromise.
* **TOMOYO**: path-based, strong learning support and simple policy model; well-suited for appliances and embedded deployments where executable paths are stable and rootfs can be protected.

---

# 12) Kernel integration & config (what you need in the kernel)

To use TOMOYO you must:

* Enable the LSM in the kernel build: typically `CONFIG_SECURITY_TOMOYO=y`.
* Enable any companion options (policy loading at early boot, userspace helper hooks) — options vary by kernel version.
* Decide whether to boot with TOMOYO in *learning* (permissive) or *enforce* mode — many systems default to learning on first boot.

> Exact Kconfig symbols and build steps vary with kernel versions and distro packaging. On embedded platforms, TOMOYO can be built into kernel or as a module depending on constraints.

---

# 13) Deployment recipe for embedded devices (recommended flow)

1. **Enable TOMOYO in kernel config** and build kernel (or get distro kernel with TOMOYO).
2. **Prepare userspace tools** for learning & policy conversion (pack `tomoyo-tools` aboard or in initramfs).
3. **Boot device in learning mode** (TOMOYO permissive). Run representative workloads and test scripts.
4. **Collect learning logs** and convert to draft policy.
5. **Review & harden policy** (narrow prefixes, remove broad wildcards, restrict capabilities and network).
6. **Persist final policy** into read-only rootfs (or a signed policy blob).
7. **Boot in enforcing mode** and monitor denies, iterate if needed.
8. **Lock device**: enable read-only rootfs, dm-verity and signed updates to prevent policy bypass.

---

# 14) Debugging & auditing

* **Logs**: TOMOYO emits denial logs (kernel log / audit) describing domain, attempted operation and path. Use dmesg / audit subsystem to inspect.
* **Learning records**: review recorded accesses and confirm whether they reflect legitimate behavior.
* **Policy testing**: maintain a test harness to exercise features and catch false positives before shipping.
* **Fallback**: keep a debug path to switch TOMOYO back to permissive mode remotely (very important for field debugging).

---

# 15) Practical mini example — train → generate → enforce (conceptual commands)

1. Boot with TOMOYO in learning/permissive.
2. Run your appliance use-cases (`start services`, `run update`, exercise GUI\`).
3. Use userspace tool to extract observed actions into a draft policy (tool: *policy generator*).
4. Inspect draft policy, tighten paths and drop any broad allows.
5. Install the policy file under `/etc/tomoyo/policy` and switch TOMOYO to enforce.
6. Monitor `dmesg` for denials and refine policy if necessary.

> If you want, I can produce a concrete step-by-step for your target (Yocto, Debian, Buildroot) including exact userspace tool names and commands. Tell me your target distro/toolchain and I’ll convert this conceptual flow into copy-paste commands and templates.

---

# 16) Best practices & hardening tips (embedded focus)

* Use TOMOYO together with **immutable rootfs** (squashfs + dm-verity) so an attacker cannot trivially replace binaries by path.
* Combine with **secure boot / signed updates** so both code and policy are validated.
* Keep policy minimal and explicit — avoid wide directory wildcards except for well-audited volatile data dirs.
* Use capabilities rules to avoid running lots of code with `CAP_SYS_ADMIN` etc.
* Keep a remote diagnostic channel to revert to permissive mode if enforcement breaks the device in the field.
* Periodically re-train policy when you change software/features and re-validate.

---

# 17) Wrap-up / next steps

TOMOYO is ideal for embedded appliances: simple semantics (path-based), strong learning workflow, and small admin overhead. It shines where the system image is stable, rootfs can be protected, and you want a quick path to a usable MAC policy.

Do you want me to:

* produce a **concrete example policy** (real TOMOYO file) for a small appliance (e.g., one daemon + log storage + update agent)?
* give **exact kernel .config snippets** and a Yocto/Buildroot integration recipe for TOMOYO?
* or generate a **policy training script** (shell + strace wrappers) you can run on your target to collect learning traces?

Pick one and I’ll produce the runnable example next.
