# Secure Boot Chain of Trust on ARM64 Deep Dive

Category: Security Extensions  
Platform: ARM64 (AArch64), TF-A plus UEFI ecosystems

---

## 1. Concept Foundation

Secure boot ensures each boot stage is authenticated before execution.

Chain-of-trust principle:
- Immutable root verifies first mutable stage.
- Each verified stage verifies the next.
- If verification fails, boot halts or falls back to recovery policy.

Why it matters:
- Prevents persistent firmware and bootloader implants.
- Protects kernel launch path and trust boundary for higher software layers.

---

## 2. ARM64 Hardware Detail

### 2.1 Root of trust anchoring

Root keys are anchored in immutable or controlled storage:
- ROM-embedded public key hash
- eFuse or OTP key hash slots
- secure element depending on platform

Hardware support often includes:
- anti-rollback version counters
- authenticated boot ROM routines
- secure storage access restrictions

### 2.2 Boot artifacts and signatures

Common verified objects:
- BL2, BL31, BL32, BL33 images
- certificate chains and metadata
- kernel and initrd in UEFI secure boot model

Cryptography:
- public key signature verification for image authenticity
- hash verification for integrity
- policy checks for key usage and lifecycle state

### 2.3 Measured boot versus verified boot

Verified boot:
- blocks execution if signature or policy fails

Measured boot:
- records measurements (hashes) into TPM or trusted log
- may allow boot but attests platform state later

Modern systems frequently combine both.

---

## 3. Linux and Firmware Implementation View

### 3.1 TF-A TBBR model

Trusted Board Boot Requirements model in TF-A typically includes:
- ROTPK (root of trust public key)
- trusted key certificates
- content certificates for images
- image signatures bound to platform policy

Boot sequence:
1. ROM verifies BL2 or first mutable loader.
2. BL2 verifies BL31, BL32, BL33 from FIP.
3. BL31 runtime secure monitor takes over and hands off safely.

### 3.2 UEFI Secure Boot path

When BL33 is UEFI:
- UEFI verifies EFI binaries (boot manager, shim, GRUB, kernel) against db and dbx policy.
- db contains allowed signer certificates.
- dbx contains revoked keys and binaries.

Linux can then enforce additional integrity policies:
- lockdown mode on secure boot systems
- module signature verification

### 3.3 Operational update flow

Secure updates require:
- signed firmware capsules or vendor update images
- version monotonicity to block rollback
- revocation updates to deny compromised keys

---

## 4. Hardware-Software Interaction

Example full chain:
1. Boot ROM authenticates BL2 using ROT hash in OTP.
2. BL2 authenticates BL31 and BL33 from FIP.
3. BL31 initializes EL3 runtime and world controls.
4. BL33 UEFI verifies next boot artifacts using db and dbx.
5. Linux boots with secure boot state exposed to OS.

Failure behavior examples:
- Signature mismatch at BL2 stage: stop before BL31 execution.
- Revoked key in UEFI dbx: deny loading binary despite valid signature structure.
- Version rollback detected: reject image even if signature is valid.

---

## 5. Interview Q and A

Q1: What is ROTPK?
Root of Trust Public Key used as trust anchor to validate the first mutable stage.

Q2: Is secure boot the same as disk encryption?
No. Secure boot verifies code integrity at startup. Disk encryption protects data confidentiality at rest.

Q3: Why do we need dbx in UEFI?
To revoke compromised signers or vulnerable binaries without replacing root keys.

Q4: Can a signed old image still be dangerous?
Yes. Without anti-rollback checks, validly signed but vulnerable old firmware can be loaded.

Q5: What is the difference between measured and verified boot?
Verified boot enforces execute or deny policy. Measured boot records evidence for attestation.

Q6: How does Linux know secure boot state?
Via firmware interfaces and platform-specific hooks that expose secure-boot mode and policy effects.

---

## 6. Pitfalls and Gotchas

- Treating signature validity as sufficient while ignoring rollback policy.
- Not updating revocation databases after key compromise.
- Inconsistent key ownership between manufacturing and field updates.
- Weak recovery process that allows unsigned rescue images.
- Forgetting to align kernel module signing policy with boot chain trust goals.

---

## 7. Quick Reference Table

| Layer | Verification responsibility |
|---|---|
| ROM | First mutable stage authentication |
| BL2 | TF-A image authentication chain |
| BL31 | Secure runtime and world control handoff |
| UEFI | EFI binary policy via db and dbx |
| Linux | Optional lockdown and module signature policies |

| Term | Meaning |
|---|---|
| ROTPK | Root trust public key anchor |
| FIP | Firmware Image Package container |
| db | Allowed signatures or certs in UEFI |
| dbx | Revoked signatures or certs in UEFI |
| Anti-rollback | Version monotonicity enforcement |
