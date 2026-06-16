Great — I’ll explain **TPM, Trusted Keys, and TEEs** from scratch for **Embedded Linux**, block-by-block, with practical notes, commands, integration patterns, and real-world best practices. I’ll highlight where each component lives (hardware, kernel, userspace), what it guarantees, how you’d use it on a device, and where they overlap. Expect conceptual background → architecture → concrete examples → integration checklist.

# High-level map (one-line)

* **TPM** = a dedicated hardware/root of trust for keys, PCRs, attestation and sealed storage. ([Trusted Computing Group][1])
* **Trusted (kernel) Keys** = kernel key types / keyring primitives that can be backed/seeded by a trust source (TPM/TEE) so secrets never appear in userspace in clear. ([Kernel.org][2])
* **TEE (Trusted Execution Environment)** = a secure runtime (secure world or separate secure co-processor) that executes trusted code and holds keys and secrets in an isolated environment (e.g., ARM TrustZone + OP-TEE). ([OP-TEE Documentation][3], [Kernel.org][4])

---

## 1) TPM (Trusted Platform Module) — what, why, core primitives

### What it is

A TPM is a small crypto co-processor (discrete chip or firmware/firm-enabled block) that provides:

* protected key storage (endorsement key etc.),
* platform measurement via **PCRs** (Platform Configuration Registers),
* sealed storage (encrypt data so it can only be unsealed when PCRs are in a given state),
* attestation (quote PCR values signed by TPM keys),
* random number generator and crypto ops. ([Trusted Computing Group][1], [Intel][5])

(Modern devices use **TPM 2.0** — more algorithms, flexible auth policies — vs the older TPM 1.2.) ([Dell][6])

### Core TPM concepts (primitives)

* **Endorsement Key (EK):** long-lived asymmetric key made/owned by the TPM vendor. Often paired with an EK certificate showing vendor attestation. ([Trusted Computing Group][1])
* **Storage Root Key (SRK)** / Primary key: a protected root under which other keys are created (logical root). (In TPM2 this is created as a primary key under hierarchy.) ([tpm2-tools.readthedocs.io][7])
* **PCRs (Platform Configuration Registers):** small registers that record measurements (hashes) produced by the boot chain (firmware, bootloader, kernel command line, kernel modules, initramfs, etc.). `PCR extend` = append a measurement. PCR values are what you quote for attestation or check when unsealing. ([Trusted Computing Group][1])
* **Sealing:** you can seal data with a policy tied to PCR values — only when the PCRs match that policy will the TPM unseal the data. Useful for binding secrets to a boot state. ([tpm2-tools.readthedocs.io][7])
* **Quote (attestation):** TPM signs PCR values (a quote) proving the measured state. Useful for remote attestation. ([Trusted Computing Group][1])

### Kernel & userspace interfaces

* Kernel exports devices like `/dev/tpm0` (one direct client) and the more modern `/dev/tpmrm0` (resource manager, safe multi-client access); kernel docs describe drivers and interfaces. Userspace libraries talk to TPM via these devices. ([Kernel Documentation][8], [ArchWiki][9])
* **Userspace tools / stacks:** `tpm2-tss` (TSS2 libraries), `tpm2-tools` (CLI), and `tpm2-pkcs11` (provide PKCS#11 token interface backed by TPM2) are de-facto on Linux. ([tpm2-tools.readthedocs.io][7], [GitHub][10])

### Typical embedded use cases

* **Measured boot & attestation:** collect measurements at boot, extend PCRs, later provide a TPM quote to a cloud service (proving device state). ([Trusted Computing Group][1])
* **Sealing secrets:** seal disk LUKS keys or application secrets such that they unseal only when the boot chain is known/trusted (PCR policy). Example: bind an encryption key to PCRs representing firmware+kernel. ([tpm2-tools.readthedocs.io][7])
* **Device identity & onboarding:** provision an attestation key (AK) and register EK/AK with the vendor/cloud to enable secure provisioning and device identity. ([Trusted Computing Group][1])

### Minimal practical TPM flow (commands — `tpm2-tools` examples)

> These are canonical examples (exact flags vary with versions). See `tpm2-tools` docs for your distro. ([tpm2-tools.readthedocs.io][7])

1. Create a primary (storage root) under the endorsement hierarchy:

```bash
tpm2_createprimary -C e -g sha256 -G rsa -c primary.ctx
```

2. Read PCRs:

```bash
tpm2_pcrread sha256:0,1,2
```

3. Seal a secret bound to PCRs (create a keyedhash/sealed object):

```bash
tpm2_create -C primary.ctx -u sealed.pub -r sealed.priv -i- < secret.bin
# then load (if needed) and unseal
tpm2_load -C primary.ctx -u sealed.pub -r sealed.priv -c sealed.ctx
tpm2_unseal -c sealed.ctx
```

(See `tpm2_unseal` manpage for the options; sealing/unsealing is covered by tpm2-tools docs.) ([tpm2-tools.readthedocs.io][7])

### Practical tips & pitfalls

* **PCR policy brittleness:** sealing to PCRs is powerful but fragile — even a small benign change in the boot chain (cmdline, module order) will change PCRs and prevent unseal. Use measured-boot design carefully (choose which PCRs to bind). ([Trusted Computing Group][1])
* **Provisioning:** EKs can be factory-provisioned; the downstream provisioning of EK certs and attestation keys must be part of a secure manufacturing process or trusted supplied-signing CA. ([Trusted Computing Group][1])
* **Resource manager:** use kernel resource manager (`/dev/tpmrm0`) or `tpm2-abrmd` / direct access carefully; modern practice is to rely on kernel `tpmrm` and `tpm2-tss` rather than systemd-abrmd. ([ArchWiki][9])

---

## 2) Trusted Keys (Linux kernel keying + Trusted & Encrypted Keys)

### What the kernel key service provides

* The **kernel key retention service** exposes keyrings to userspace and kernel subsystems via `keyctl`/`keyutils` and syscalls. This is a generic mechanism to hold keys/credentials in kernel memory without exposing them to arbitrary processes. ([Kernel.org][11])

### "Trusted Keys" and "Encrypted Keys" kernel feature

* Linux added two special key types: **Trusted Keys** and **Encrypted Keys** (variable length symmetric keys produced and managed by kernel).

  * **Encrypted Keys**: allow userland to store/load encrypted blobs; they’re portable and don’t require a hardware trust source.
  * **Trusted Keys**: similar to Encrypted Keys but **require a trusted hardware source** (e.g., TPM or TEE) so the kernel can decrypt the blob only when the trust source allows it. This lets kernel subsystems (dm-crypt, kernel crypto) use keys that are never exposed plaintext to userspace. ([Kernel.org][2], [dri.freedesktop.org][12])

### How this appears on an embedded device

* Example: **dm-crypt** uses the kernel key retention service to hold the LUKS/LVM key; a Trusted Key can be used so the key only exists (plaintext) inside kernel memory and is protected by TPM/TEE unsealing — userspace never directly sees the raw key. This is the “trusted key” pattern for full-disk encryption tied to hardware. ([Timesys][13], [dri.freedesktop.org][12])

### Integration patterns / tooling

* **systemd** and related tooling: systemd supports `systemd-cryptenroll` and has options to store disk keys in TPM2/TEE backends (using tpm2-tools, tpm2-tss, or PKCS#11 layers). `tpm2-pkcs11` is a commonly used bridge to present TPM keys via a PKCS#11 interface so existing PKCS#11-aware apps can use TPM-protected keys. ([GitHub][10], [Azure][14])

### Practical notes

* **Kernel config & userspace:** to use Trusted Keys you need the kernel options and userspace support (the backends that perform protection: TPM drivers, TEE drivers, or vendor HSM drivers). See kernel docs for `trusted/encrypted keys`. ([Kernel.org][2])
* **Security boundary:** Trusted Keys keep secrets out of userspace; provisioning and backup of the blobs still needs a secure process (manufacturing keys, attestation, redundancy).

---

## 3) TEE (Trusted Execution Environment) — what and how it differs from TPM

### What a TEE is (concept)

A **TEE** is a secure execution environment providing confidentiality and integrity for code & data. On ARM SoCs this is typically **TrustZone** (secure world + normal world). Common open TEE implementations for embedded Linux: **OP-TEE** (open source) which implements GlobalPlatform TEE APIs. TEEs let you run Trusted Applications (TAs) that hold secrets, perform crypto, and serve the normal world through a Client API. ([OP-TEE Documentation][3])

### Key differences vs TPM

* **TPM** is *hardware (or firmware) dedicated* to key storage, PCRs, and attestation. Its API is command/response oriented. TPM is excellent for device identity, non-volatile protected keys, and attestation. ([Trusted Computing Group][1])
* **TEE** is a *secure runtime* that can hold keys in memory and run arbitrary trusted code. TEEs are great for cryptographic operations (where keys must not leave the TEE), secure UI, commercial DRM, Keymaster/keystore in Android, and for performing complex attestation flows. TEEs can generate keys inside secure memory and do signing without exposing private keys to the REE (Rich Execution Environment). ([GlobalPlatform][15], [OP-TEE Documentation][16])

### TEE architecture & Linux integration

* On Linux the **TEE subsystem** provides a generic driver model and shared memory handling between normal world apps and the secure world. The **OP-TEE** driver (drivers/tee/optee) plus `tee_client_api` library and `tee-supplicant` are the usual stack on ARM TrustZone devices. The client application calls into the kernel TEE driver, which performs an SMC to the secure monitor/OP-TEE OS to dispatch into the TA. ([Kernel.org][4], [OP-TEE Documentation][16])

### Use cases in embedded devices

* **Key storage / Keymaster (Android):** keys are generated and stored in the TEE and cryptographic operations are performed there, e.g., Android Keymaster runs in TEE. This makes key extraction from a compromised Linux unlikely. ([OP-TEE Documentation][17])
* **Secure UI and biometric ops**, **payment**, **certificate provisioning**, **attestation** where the TEE produces an attestation token signed by a TEE-resident key. ([OP-TEE Documentation][3])

### Example flow (OP-TEE)

1. Client app calls TEE Client API to open a session with a Trusted Application (TA) (identified by UUID). ([OP-TEE Documentation][17])
2. The TEE client library → kernel TEE driver → secure monitor / OP-TEE OS route the request to the TA. ([OP-TEE Documentation][16])
3. TA performs the sensitive operation (key generation, sign), returns result. Secret keys never leave the TA. ([OP-TEE Documentation][3])

---

## 4) Putting them together — integration patterns for Embedded Linux

### Common architecture patterns

1. **Measured boot + TPM attestation + TEE for runtime key ops**

   * Bootloader measures firmware/kernel/initramfs → extends PCRs in TPM. Kernel/IMA may measure further. At runtime you can ask TPM to `quote` PCRs for remote attestation. Use TEE to store device private keys and to perform application cryptography without exposing keys to Linux. TPM provides persistent identity; TEE provides secure runtime. ([Kernel Documentation][8], [Kernel.org][4])

2. **Sealed secrets for disk encryption (TPM)**

   * Create a disk encryption key (LUKS); seal it to PCRs in TPM. At boot, TPM unseals only if PCRs match expected measurements — otherwise disk key remains sealed and disk stays inaccessible. Use Trusted Keys in kernel so the LUKS key never leaves kernel space. ([Timesys][13], [Kernel.org][2])

3. **PKCS#11 / HSM abstraction for apps**

   * Use `tpm2-pkcs11` to expose TPM-backed keys as a PKCS#11 token so existing TLS stacks/SSH clients can use the TPM for private key ops without changing much application code. ([GitHub][10])

4. **Device onboarding & remote attestation**

   * Provision EK/AK during manufacturing; device obtains a signed quote and the cloud verifies PCRs and EK cert chain; cloud provides device-specific provisioning (secrets, certificates). ([Trusted Computing Group][1])

### Practical integration checklist (embedded build)

* Kernel: enable TPM drivers (`CONFIG_TPM` and specific TIS/CRB drivers), `CONFIG_SECURITY` options you need, and TEE driver (OP-TEE driver) if using TrustZone. Mount `smackfs` etc. (See kernel TPM/TEE docs.) ([Kernel Documentation][8], [Kernel.org][4])
* Userspace packages: `tpm2-tss`, `tpm2-tools`, `tpm2-pkcs11` (optional), `optee` (TA/OS) & `tee-supplicant` if using OP-TEE. ([tpm2-tools.readthedocs.io][7], [GitHub][10], [OP-TEE Documentation][16])
* Provisioning: decide EK/AK handling (manufacturer vs operator), plan PCR policy selection, implement secure provisioning of initial keys and certificates. ([Trusted Computing Group][1])

---

## 5) Best practices & hardening tips (practical)

* **Design PCR strategy carefully** — choose which PCRs to bind to; minimize unnecessary PCR bindings to avoid accidental lockouts. ([Trusted Computing Group][1])
* **Keep private keys out of userspace** — use Trusted Keys / TEE or kernel keyring so plaintext keys never appear in userland. ([Kernel.org][2])
* **Use tpm2-pkcs11** to leverage PKCS#11 apps and avoid rewriting TLS/SSH stacks. ([GitHub][10])
* **Provision securely** — EKs and EK certs are sensitive; manufacturing and ownership transfer must be considered. ([Trusted Computing Group][1])
* **Test recovery paths** — builds change; test how you re-provision or recover keys if you need to update boot components (PCR changes). ([tpm2-tools.readthedocs.io][7])
* **Use a hardware-backed RNG seed** — letting the TPM seed the OS entropy early can help cryptographic strength. ([Trusted Computing Group][1])

---

## 6) Quick reference & further reading (authoritative docs)

* TCG “Trusted Platform Module 2.0: A Brief Introduction”. ([Trusted Computing Group][1])
* Linux kernel TPM documentation (drivers, UAPI). ([Kernel Documentation][8])
* `tpm2-tools` manual (seal/unseal, create/load operations). ([tpm2-tools.readthedocs.io][7])
* `tpm2-pkcs11` project (PKCS#11 bridge for TPM2). ([GitHub][10])
* OP-TEE / GlobalPlatform TEE docs and Linux TEE subsystem. ([OP-TEE Documentation][3], [Kernel.org][4])
* Linux kernel “Trusted and Encrypted Keys” docs for kernel key types. ([Kernel.org][2])

---

If you want, I can now do one of these immediately (pick one — I’ll produce it right away):

1. A **copy-paste TPM lab**: buildroot/Yocto or host commands that create a primary key, seal a small secret to PCRs, then reboot and unseal — includes exact `tpm2-tools` commands and notes for `/dev/tpmrm0`. ([tpm2-tools.readthedocs.io][7], [ArchWiki][9])
2. A **TPM + TEE integration example**: show how to store a TLS private key in OP-TEE (Trusted Application) and how the Linux side calls it (client API) — with example code snippets and OP-TEE build hints. ([OP-TEE Documentation][16])
3. A **Yocto recipe snippet** and kernel config fragment\*\* to enable TPM2 and OP-TEE on an ARM board, plus initial provisioning script to apply file labels and load a PCR-based sealed key. ([Kernel Documentation][8], [OP-TEE Documentation][16])

Which one do you want me to produce next?

[1]: https://trustedcomputinggroup.org/resource/trusted-platform-module-2-0-a-brief-introduction/?utm_source=chatgpt.com "Trusted Platform Module 2.0: A Brief Introduction"
[2]: https://www.kernel.org/doc/html/v4.13/security/keys/trusted-encrypted.html?utm_source=chatgpt.com "Trusted and Encrypted Keys — The Linux Kernel documentation"
[3]: https://optee.readthedocs.io/en/latest/general/about.html?utm_source=chatgpt.com "About OP-TEE"
[4]: https://www.kernel.org/doc/html/v6.1/staging/tee.html?utm_source=chatgpt.com "TEE subsystem — The Linux Kernel documentation"
[5]: https://www.intel.com/content/www/us/en/learn/what-is-a-trusted-platform-module.html?utm_source=chatgpt.com "What Is a Trusted Platform Module (TPM)? - Intel"
[6]: https://www.dell.com/support/kbdoc/en-us/000131631/tpm-1-2-vs-2-0-features?utm_source=chatgpt.com "TPM 1.2 vs 2.0 Key Differences and Features | Dell US"
[7]: https://tpm2-tools.readthedocs.io/en/latest/man/tpm2_unseal.1/?utm_source=chatgpt.com "tpm2_unseal - tpm2-tools - Read the Docs"
[8]: https://docs.kernel.org/security/tpm/index.html?utm_source=chatgpt.com "Trusted Platform Module documentation"
[9]: https://wiki.archlinux.org/title/Trusted_Platform_Module?utm_source=chatgpt.com "Trusted Platform Module - ArchWiki"
[10]: https://github.com/tpm2-software/tpm2-pkcs11?utm_source=chatgpt.com "tpm2-software/tpm2-pkcs11: A PKCS#11 interface for TPM2 hardware"
[11]: https://www.kernel.org/doc/html/v4.20/security/keys/core.html?utm_source=chatgpt.com "Kernel Key Retention Service"
[12]: https://dri.freedesktop.org/docs/drm/security/keys/trusted-encrypted.html?utm_source=chatgpt.com "Trusted and Encrypted Keys — The Linux Kernel documentation - DRI"
[13]: https://www.timesys.com/security/linux-kernel-security-dm-crypt-with-trusted-keys-using-tee-backend/?utm_source=chatgpt.com "Linux Kernel Security: dm-crypt with \"trusted keys\" using TEE backend"
[14]: https://azure.github.io/iot-identity-service/pkcs11/tpm2-pkcs11.html?utm_source=chatgpt.com "Installing and configuring the tpm2-pkcs11 library for TPM 2.0 TPMs"
[15]: https://globalplatform.org/specs-library/tee-internal-core-api-specification/?utm_source=chatgpt.com "TEE Internal Core API Specification v1.3.1 - GlobalPlatform"
[16]: https://optee.readthedocs.io/en/stable/architecture/core.html?utm_source=chatgpt.com "Core - OP-TEE Documentation - Read the Docs"
[17]: https://optee.readthedocs.io/en/latest/architecture/globalplatform_api.html?utm_source=chatgpt.com "GlobalPlatform API - OP-TEE Documentation - Read the Docs"
