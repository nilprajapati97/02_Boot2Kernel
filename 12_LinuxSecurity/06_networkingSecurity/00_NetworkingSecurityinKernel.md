# Networking Security in Kernel — block-wise graphical deep dive

This document explains **Netfilter / nftables**, **seccomp-bpf**, **MACsec / IPsec**, and **TLS offload (kTLS / NIC offload)** from the Linux kernel point of view. Each block contains: **what & why**, **threats**, **internal flow (ASCII block diagram)**, **how to implement / kernel APIs & examples**, **performance & tradeoffs**, and **best practices**.

---

# Netfilter / nftables (Kernel firewall)

**What & why**

* Kernel-level packet filtering, NAT and connection tracking. `nftables` is the modern userspace/front-end that programs in-kernel filtering tables via netlink; it replaces the older `iptables`/`ip6tables`/`ebtables` chains.
* Use it to drop/accept packets, NAT (SNAT/DNAT), rate-limit, and implement stateful firewalling.

**Threats addressed**: unauthorized inbound services, packet spoofing, DoS mitigation (rate limits), unintended forwarding/NAT leaks.

**Internal flow (simplified packet path)**

```
[NIC RX] --> XDP (early, driver layer: drop/redirect) --> skb alloc --> NF_PREROUTING --> conntrack --> routing decision
    --> if local dest: NF_INPUT --> socket layer --> userspace
    --> if forward: NF_FORWARD --> NF_POSTROUTING (NAT) --> NIC TX

Hooks: PREROUTING, INPUT, FORWARD, OUTPUT, POSTROUTING
Conntrack sits between PREROUTING and routing decision to track state.
```

**How it works / kernel APIs**

* Userspace programs (`nft`) talk to kernel via **netlink** (nf\_tables subsystem).
* Kernel has hook points (netfilter hooks) where rules are evaluated.
* Connection tracking (`nf_conntrack`) records flows to implement stateful rules and NAT.
* Flow offload / hardware offload can be used to push established flows to NIC or hardware accelerators.

**Example (nftables minimal ruleset)**

```bash
# create a table and chains
nft add table inet fw
nft 'add chain inet fw input { type filter hook input priority 0; policy drop; }'
# allow established/related
nft add rule inet fw input ct state established,related accept
# allow SSH
nft add rule inet fw input tcp dport 22 ct state new accept
```

**Performance & tradeoffs**

* XDP is the earliest and fastest place to drop packets (runs at driver level, before skb alloc). Use for high-performance DDoS dropping.
* nftables with flow offload and conntrack reduces per-packet rule evaluations after flow is offloaded.
* More rules / complex sets can increase lookup cost — prefer sets/maps and flow-offload where possible.

**Best practices**

* Use `nftables` (not legacy iptables) for new designs.
* Place coarse-grain rejects early (XDP / prerouting) and finer policies later.
* Track state with conntrack for NAT and filtering but be wary of conntrack table exhaustion (use per-IP limits).
* Use nf\_conntrack helpers sensibly (FTP, SIP) only when needed.

---

# Seccomp‑BPF (syscall sandboxing)

**What & why**

* Seccomp‑BPF filters limit what syscalls a process may invoke, implemented in kernel as a Berkeley Packet Filter (BPF) program evaluated on each syscall entry.
* Prevents exploitation techniques that require certain syscalls (e.g., `ptrace`, `mount`, `reboot`, `kexec`, `clone` variants).

**Threats addressed**: post-exploit lateral movement, privilege escalation paths requiring specific syscalls, reducing attack surface of network-exposed daemons.

**Internal flow (syscall path)**

```
[User process] --(syscall entry)--> kernel seccomp hook --> run BPF program
    --> BPF returns action: ALLOW / KILL / ERRNO / TRACE / NOTIFY
    --> If NOTIFY: kernel queues notification to a user-mode handler (user-notif) which can decide
```

**How to implement**

* Two modes: strict (very limited) and filter (BPF). Modern sandboxes use filter mode via `prctl` or `seccomp` syscall.
* Use `libseccomp` from userspace for easier rule creation. Example (pseudo-code using libseccomp):

```c
scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL);
seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 0);
seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(exit), 0);
seccomp_load(ctx);
```

* Newer feature: `SECCOMP_RET_USER_NOTIF` allows a supervising process to get notified when a filtered syscall occurs, enabling policy decisions in userspace.

**Performance & tradeoffs**

* BPF is fast but evaluated on every syscall — heavy syscall workloads may see overhead.
* Seccomp is best used to restrict high-value daemons (network-facing) rather than general system processes.

**Best practices**

* Design least-privilege syscall lists (start wide during development, then harden).
* Combine seccomp with namespaces and capabilities for layered defense.
* Use user-notif when you need selective, on-demand passthrough for complex operations.

---

# MACsec (802.1AE) vs IPsec (L2 vs L3 encryption)

**What & why**

* **MACsec**: link-layer (L2) frame protection — confidentiality + integrity on Ethernet links between switches/hosts.
* **IPsec**: network-layer (L3) IP packet protection (ESP/AH) providing host-to-host, gateway-to-gateway, or gateway-to-host tunnels.

**Threats addressed**: on-path eavesdropping, tampering, and replay on the link (MACsec) or across routed networks (IPsec).

**Block diagrams**

* *MACsec (L2)*

```
Host A NIC (MACsec) <--- encrypted Ethernet frames ---> Switch (MACsec capable) <--- encrypted ---> Host B NIC
Encryption happens on each Ethernet frame before it's handed to the wire.
```

* *IPsec (L3)*

```
Host A IP stack --(IPsec encapsulate/ESP)--> Wire (encrypted IP) --> Host B --(decapsulate)--> IP stack
Modes: transport (host-to-host payload encrypted) or tunnel (entire IP packet encapsulated)
```

**Kernel components & APIs**

* **MACsec**: kernel `macsec` module + netdevice-level integration. Admin via `ip link add link eth0 macsec0 type macsec` and `ip macsec` commands or userspace helpers.
* **IPsec**: kernel `xfrm` framework (policy & state), SAD (Security Association Database) and SPD (Policy Database). Userspace managers (strongSwan, libreswan) program xfrm via netlink or PF\_KEY.

**Example (high-level)**

* MACsec: create a MACsec netdev, provision keys (CAK/CKN), set connectivity and policy — kernel handles frame encryption/decryption.
* IPsec: userspace establishes SAs (IKEv2 via strongSwan) and kernel xfrm applies encryption with the chosen algorithms (ESP/AES-GCM).

**Performance & tradeoffs**

* MACsec is transparent to IP and performs at line rate if supported by hardware (wire-speed switches/ASIC). It is ideal for LAN links where both ends and path devices support MACsec.
* IPsec is more flexible (works across routed networks) but may be heavier CPU-wise unless NIC offload is available.
* Hardware offload (NIC crypto engines) can greatly improve throughput; kernel supports offload hooks and drivers to push crypto to NIC.

**Best practices**

* Use MACsec for protected LAN links when possible (simpler, lower overhead). Ensure switch path supports it.
* For wide-area protection and VPNs, use IPsec with strong ciphers (AEAD like AES-GCM or ChaCha20-Poly1305) and keep SA lifetimes and replay windows tuned.
* Monitor SA counters and rekey proactively.

---

# TLS offload in Kernel (kTLS & NIC offload)

**What & why**

* **kTLS**: kernel-side TLS processing where TLS record encoding/decoding is performed in kernel TCP stack instead of entirely in userspace. This reduces copies/context switches and allows direct integration with kernel TCP features.
* **NIC TLS offload**: NICs that understand TLS can perform crypto on TLS records for further speedups (hardware TLS offload).

**Internal flow (high-level)**

```
[User App / TLS lib] --(TLS handshake, keys)--> kernel TLS context (kTLS)
    -> kernel TCP stack handles TLS record processing for TX/RX
    -> optionally: kernel hands crypto parameters / descriptors to NIC driver -> NIC encrypts/decrypts on the wire
```

**How to enable / where it fits**

* Applications (or TLS libraries) that support kTLS can export session keys to the kernel so the kernel can encrypt/decrypt TLS records. This is typically negotiated by userspace (OpenSSL/boringssl) then kernel is configured with keys.
* kTLS handles record-layer operations; for NIC offload, the kernel/driver must support TLS device offload and supply descriptors to the NIC.

**Benefits & tradeoffs**

* Benefits: fewer copies, lower CPU per connection, better throughput when many TLS connections exist. Enables zero-copy send/receive and reduces latency.
* Tradeoffs: moving keys into kernel / NIC increases the trusted computing base. Drivers and kernel TLS code must be secure. Hardware offload requires trusted NIC firmware/drivers.

**Best practices**

* Use kTLS for high-throughput servers (e.g., reverse proxies) after auditing trust boundaries. Prefer TLS offload only on trusted hardware and with validated drivers.
* Provide fallback to userspace TLS if kTLS or hardware offload not available.

---

# Cross-cutting considerations

* **Layering**: Combine early-drop (XDP) + nftables + seccomp + namespaces to get defense-in-depth.
* **Offload carefully**: offload improves perf but increases attack surface. Validate hardware/firmware and have fallbacks.
* **Monitoring & limits**: watch conntrack table sizes, seccomp deny logs, IPsec SA lifetimes and rekey events.

---

If you want, I can:

* convert any ASCII block into an SVG/PNG diagram; or
* expand any block into a full working example (nftables + XDP drop example, full libseccomp sample with compile instructions, ip xfrm commands + strongSwan conf, or kTLS short demo).

Which block should I expand into runnable examples or diagrams next?
