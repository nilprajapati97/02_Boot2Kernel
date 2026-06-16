Nice — I’ll break each item into a self-contained “block” so you can absorb it step-by-step. For each block I’ll cover: **what it is**, **why it matters (threat model)**, **how it works internally**, **how to implement (practical steps & examples)**, **testing / pitfalls**, and **best practices**. Let’s go.

# 1) Encrypted rootfs — **dm-crypt / LUKS (block-level) & fscrypt (file-level)**

## What / Why

* **Block-level encryption (dm-crypt / LUKS)** encrypts the whole block device (partition). Good when you want *all* data on that partition unreadable without the key (e.g., root filesystem, swap, data partitions).
* **File-level encryption (fscrypt)** encrypts individual files/directories inside a filesystem (supported on ext4, f2fs). Good when you want multi-user file confidentiality, or to avoid encrypting whole device.

Threats addressed: theft of storage media, offline attacks (attacker removes eMMC/SSD and reads it), unwanted disclosure if device is physically captured.

## Internals — how they work

* **dm-crypt/LUKS**

  * `dm-crypt` is a Linux device-mapper target. It sits under the filesystem: filesystem → block device (/dev/mapper/cryptX) → dm-crypt → underlying block device.
  * **LUKS** is a standard on-top of dm-crypt that defines header format, keyslots, metadata, and passphrase/key management. A random *volume key* (data-encryption key) encrypts the device. That key is itself encrypted by one or more *keyslots* (each keyslot protected by a passphrase or key file).
  * On open, `cryptsetup` decrypts a keyslot to recover the data key, which is provided to dm-crypt and used to derive encryption keys for the device.
  * Uses kernel crypto API (can leverage AES-NI / ARM crypto extensions) and cipher modes such as XTS for disk encryption.

* **fscrypt**

  * Implements transparent per-file or per-directory encryption at the filesystem layer. Each file gets its own encryption context derived from a directory policy key. The FS stores metadata (flags / key IDs) in filesystem structures/extended attributes; encryption/decryption happens in VFS layer.
  * File data on disk is encrypted block-by-block; metadata still sits in FS structures (name encryption requires separate features).

## How to implement (practical)

### LUKS (full-root example)

1. Prepare partition (e.g., `/dev/mmcblk0p2`) and create LUKS container:

   ```bash
   # (interactive) creates LUKS header and keyslot
   sudo cryptsetup luksFormat /dev/mmcblk0p2
   # open it and map as /dev/mapper/cryptroot
   sudo cryptsetup open /dev/mmcblk0p2 cryptroot
   # make filesystem and mount
   sudo mkfs.ext4 /dev/mapper/cryptroot
   sudo mount /dev/mapper/cryptroot /mnt
   ```
2. Add keyslots for passphrase / keyfile:

   ```bash
   sudo cryptsetup luksAddKey /dev/mmcblk0p2 /root/keyfile
   ```
3. Boot unlocking:

   * Use initramfs script to prompt for passphrase and `cryptsetup open …`.
   * Prefer hardware-backed unlocking: use TPM/cleartext-protected key via tools (e.g. clevis, systemd-cryptenroll) so device auto-unlocks only on trusted boot.

### fscrypt (per-directory)

1. Ensure filesystem supports fscrypt and is mounted with the right options (ext4 or f2fs).
2. Initialize and create protector keys (example with `fscrypt` userland tool):

   ```bash
   sudo fscrypt setup /               # create metadata areas
   sudo fscrypt encrypt /home/alice   # interactive: create protector and encrypt dir
   ```

   (exact commands depend on fscrypt tool version).

## Key management & sealing

* **Never store the plain data key on disk**. Common patterns:

  * User enters passphrase at boot (simple).
  * **TPM / TEE / HSM**: seal/unseal the LUKS key or keyfile to TPM PCRs (measured boot). Use clevis/tang or systemd tools to auto-unlock if platform state matches.
  * **Key in initramfs**: easy but weak — if initramfs is modifiable by attacker it’s compromised.
* LUKS supports multiple keyslots — you can rotate passphrases/keyfiles without re-encrypting data.

## Performance & tuning

* Block-level encryption (dm-crypt) is typically fast when hardware crypto is available (AES-NI, ARM crypto). Use the kernel crypto API cipher that matches hardware. XTS mode is common for disk.
* For small random I/O (databases), encryption adds latency; benchmark for your workload.
* Align encryption with underlying device block size to avoid performance penalties.

## Testing & pitfalls

* **Back up LUKS header!** If header corrupts, you lose access unless you have backups of header and keys. `cryptsetup luksHeaderBackup` is essential.
* If you lose the passphrase and keyslots don't contain another key, data is unrecoverable (by design).
* If you use TPM/unsealing, test cold boot and recovery paths thoroughly.

## Best practices (short)

* Use **LUKS2** (better metadata & features) where available.
* Protect header backups and protect private keys offline.
* Use hardware-backed key storage (TPM/TEE) for unattended devices.
* Keep immutable read-only root and only encrypt user data partitions if possible to simplify unlocking.

---

# 2) Read-only rootfs (squashfs, overlayfs, A/B) — block-wise

## What / Why

A read-only root filesystem prevents runtime tampering of system binaries/configuration and reduces attack surface and accidental corruption. Common in appliances and automotive. Updates are done atomically (A/B slots) or via signed image replacement.

Threats addressed: runtime tampering, accidental write damage, persistence of injected malware after reboot.

## Internals — typical patterns

* **SquashFS (read-only compressed image)**: a compressed read-only filesystem image that is mounted as root; compact and verifiable.
* **OverlayFS / unionfs**: used to present a read-only base (lowerdir) with a writable upperdir (tmpfs or separate partition). Useful for allowing transient writes while keeping base immutable.
* **A/B partitioning (dual root)**: two copies (slots) of root; update writes to the inactive slot; after verification, bootloader switches active slot. Provides atomic update and rollback.

## How to implement (practical)

### Simple read-only squashfs + overlay ram

1. Create squashfs image:

   ```bash
   mksquashfs rootfs/ rootfs.sqsh -comp xz
   ```
2. Boot mounts (pseudo):

   ```bash
   mount -t squashfs /dev/mmcblk0p3 /lower
   mount -t tmpfs tmpfs /upper
   mkdir /upper/work
   mount -t overlay overlay -o lowerdir=/lower,upperdir=/upper,workdir=/upper/work /sysroot
   pivot_root /sysroot /sysroot/oldroot   # or switch root via initramfs
   ```

   In this setup, changes are stored in RAM (tmpfs) and disappear on reboot — useful for immutable appliances.

### A/B updates (atomic)

* Partition layout: slotA (rootfs A), slotB (rootfs B), bootloader env holds active slot.
* Update flow:

  1. Write new image to inactive slot.
  2. Verify signature/digest of new slot.
  3. Update bootloader env to point to new slot and reboot.
  4. On boot, verify the new system; on success, mark slot good. If boot fails, bootloader falls back to other slot.

## Handling state (logs, persistent config)

* Put `/var`, `/etc/volatile` , `persist/` on separate writable partitions. Use overlay for parts of `/etc` that can change. Use `systemd` or custom init scripts to migrate/read config from persistent partition into overlay.

## Pros / Cons

* * Strong immutability and integrity.
* * Easier rollback and recovery strategies.
* − Need to design where persistent data lives.
* − Update system complexity (slot management, rollback counters).

## Best practices

* Use signed images combined with A/B for safe atomic updates.
* Use dm-verity on read-only partitions (see next block) to protect against bit flips/tamper.
* Ensure persistent state is stored on distinct partition(s) and handled explicitly during updates.

---

# 3) IMA (Integrity Measurement Architecture) & EVM — deep dive

## What / Why

* **IMA** measures files (hashes) when they are read/executed and can optionally *appraise* (verify signatures) or *log* those measurements. It can be used for local enforcement and remote attestation (prove system boot state).
* **EVM** (Extended Verification Module) protects integrity of extended attributes and prevents tampering of those xattrs (the storage location where signatures / ima values are kept). EVM ensures that an attacker cannot just overwrite xattrs to bypass IMA.

Threats addressed: runtime tampering of binaries, stealthy replacement of applications, undetected filesystem tampering.

## Internals — how IMA/EVM operate

* IMA hooks into kernel VFS operations (open, exec, read depending on policy). When a file subject to a rule is accessed, IMA computes a hash of the file contents and:

  * **Measure**: logs the hash to the kernel measurement list — can be extended into a TPM PCR (PCRs capture boot state).
  * **Appraise**: checks if the file is accompanied by a valid signature or hash in an allowed store; if appraisal fails, action depends on policy: deny access/execute or just log.
* **EVM** protects the integrity of critical filesystem metadata and extended attributes (e.g., xattrs used to store signatures). By signing xattrs and protecting them with EVM, you prevent attackers from swapping in modified file and also updating attributes to match.

IMA can be integrated with TPM: measurements can be extended into PCR registers; a remote verifier can request a TPM quote (signed PCR values) to attest to the system state.

## How to implement (conceptual steps)

1. **Kernel support**: enable IMA/EVM and required options in the kernel (IMA, appraisal, TPM integration if using TPM). Rebuild kernel or enable via distro that supports it.
2. **Policy**: write an IMA policy that lists which files to measure or appraise (e.g., measure all files in `/usr/bin`, appraise kernel modules, appraise boot scripts, etc.). Policy grammar is expressive (measure/appraise rules).
3. **Sign binaries**:

   * Create a signing keypair (offline).
   * Sign target file(s) with vendor private key; store signature as extended attribute (or in a signature store depending on mode). Tools like the IMA utilities (`evmctl`, `efi-sig` utilities, distro-specific tools) are used to sign and set xattrs.
4. **Appraisal mode**: configure IMA to appraise files. If a binary lacks the correct signature, kernel will refuse to execute it (depending on policy).
5. **TPM integration / attestation**: IMA extends measurements into TPM PCRs; remote attestation uses `tpm2_quote` to prove PCR content; remote verifier checks expected measurements.

## Example (high-level)

* Vendor signs `/usr/bin/daemon` using private signing key; sets signature into file xattr. At boot, kernel IMA—appraisal sees `daemon` executing: it computes hash, checks signature against vendor public key in kernel keyring, allows execution only if valid. The same measurement can be extended to TPM PCRs for remote verification.

## Testing / pitfalls

* **Policy complexity**: overly broad appraisal can brick system (if e.g., /sbin/init is appraised and signatures are missing). Test on development builds.
* **Key management**: public keys used for appraisal must be trusted and stored securely (kernel keyring, built into initramfs/bootloader). If keys are compromised, the system is compromised.
* **Performance**: measuring/appraising all file reads can add overhead; carefully decide the scope (measure only executables or critical files).
* **EVM vs. xattr stores**: ensure filesystem supports xattrs and they are not stripped by tools or transfers.

## Best practices

* Start with **measure-only** mode, collect logs, then move to **appraise** after you have signatures for everything required.
* Use a **TPM-backed key** for attestation where possible so that private keys never leave hardware.
* Keep signing private keys offline and rotate per release.

---

# 4) Firmware / OTA update signing (secure update flow) — block-wise

## What / Why

OTA signing is the practice of cryptographically signing firmware/rootfs/update bundles so the device only installs authentic, vendor-approved images. Combined with atomic update strategies and rollback protection, it prevents attackers from pushing malicious or downgraded firmware.

Threats addressed: remote compromise via software updates, man-in-the-middle update injection, rollback attacks.

## Core components of a secure OTA design

1. **Signed update artifact** (image + metadata + signature).
2. **Verifier on device** (bootloader or update agent) that validates signature prior to applying or booting.
3. **Atomic update strategy**: A/B partitions, or transactional update + verified reboot.
4. **Rollback protection**: monotonic counter stored in secure storage (TPM, secure flash, bootloader preserved variable) to prevent installing older signed images.
5. **Key management**: public verification key(s) stored securely on device (immutable in bootloader or fuse/TPM). Private signing key is kept offline by vendor.

## How it works (step-by-step, safe update)

1. Vendor builds `update.img` and computes digest (+ metadata like version, slot) and **signs** the digest using vendor private key (RSA/ECDSA).
2. Device downloads `update.img` and signature. The **update client** or **bootloader** verifies the signature using stored vendor public key. If signature fails → reject.
3. Write image to inactive slot (A/B) or apply update atomically to a staging area.
4. Verify on-disk integrity (optional: fs-verity/dm-verity of resulting partition).
5. Update bootloader active-slot marker to new slot and set a "pending" marker. Reboot.
6. On first boot into new slot, verify integrity and start health checks — on success, mark slot as good; on failure, bootloader falls back to previous slot.

## Common implementation pieces & tools

* **Bootloader verification**: U-Boot FIT images can be signed. Bootloader verifies the kernel/initramfs before booting.
* **Update frameworks**: RAUC, Mender, SWUpdate, OSTree — they implement signed updates, A/B, rollback counters and verification (choose based on system needs).
* **Signature verification**: can be done via OpenSSL or embedded libs. Example verification command (conceptual):

  ```bash
  openssl dgst -sha256 -verify pub.pem -signature update.sig update.img
  ```

  (real systems use more structured manifests and metadata.)

## Rollback protection & anti-rollback

* Use a **monotonic counter** stored in a secure store (TPM NV, protected flash, secure element). Each update includes a monotonically-increasing version/counter. Device rejects images with version ≤ stored counter.
* Ensure the counter itself cannot be reset by attacker (store in TPM or bootloader area that survives reflashes or is signed).

## Secure key provisioning

* **Public key** for verification should be baked into immutable storage: the bootloader, ROM, or OTP fuses. This prevents replacing the verification key via software update.
* Keep the **private signing key** offline and well-protected (HSM recommended).

## OTA over network — transport security

* Use TLS (mutual if possible) to download update payloads. However, transport security is *not* a substitute for artifact signing — always verify signature locally.

## Testing & failure modes

* Test power loss during update — A/B with atomic switch covers this.
* Test rollback in all failure modes.
* Monitor and log update attempts and verification failures for security monitoring.

## Best practices (short)

* Sign artifacts with vendor private key and verify before writing any flash.
* Use A/B or transactional update + bootloader verification.
* Have a robust rollback policy backed by secure monotonic counters.
* Keep verification public keys immutable and separate from update process.

---

# Final practical checklist (quick recap)

* For **confidentiality** (data at rest) → use **LUKS + TPM-sealed keys** for unattended devices; use **fscrypt** for per-user file confidentiality.
* For **immutability / tamper-resistance** → use **read-only rootfs (squashfs)** + **dm-verity** + **A/B updates**.
* For **runtime integrity** → enable **IMA/EVM** (measure & appraise) and integrate with **TPM** for attestation. Start with measure-only then move to appraise.
* For **secure updates** → sign updates, verify in bootloader/update client, use atomic A/B, and protect against rollback.
* Use hardware roots (TPM/TEE/HSM) where possible; keep private signing keys offline.

---

If you want, I can now:

* produce a **concrete example** (step-by-step) that combines these: e.g., build a signed squashfs root with dm-verity + LUKS data partition + IMA appraisal + an A/B OTA flow (with commands and U-Boot/bootloader steps);
* or give a **kernel .config** snippet and exact `cryptsetup` / `fscrypt` / `evmctl` commands for a tested flow you’d like to deploy (specify target distro/board).

Which one next — a full worked example (commands + files) or a compact checklist for integration into your build pipeline?
