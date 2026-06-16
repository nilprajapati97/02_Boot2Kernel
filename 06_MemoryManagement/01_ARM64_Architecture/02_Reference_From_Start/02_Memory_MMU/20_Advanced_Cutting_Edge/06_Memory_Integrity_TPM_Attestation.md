# Memory Integrity, TPM, and Remote Attestation Deep Dive

Category: Advanced and Cutting-Edge Topics  
Platform: ARM64 (AArch64), memory measurement and verification

---

## 1. Concept Foundation

Memory integrity verification ensures code/data hasn't been tampered.

Mechanisms:
- cryptographic measurement (hashing)
- TPM (Trusted Platform Module) for secure measurement storage
- remote attestation for verification by external parties

---

## 2. ARM64 Hardware Detail

### 2.1 TPM integration

ARM servers can integrate TPM (TPM 2.0 standard).
Provides secure key storage and crypto operations.

### 2.2 Hardware Root of Trust

ARM TrustZone or secure processor serves as root.
All measurements ultimately verified through secure world.

---

## 3. Linux Kernel Implementation

### 3.1 IMA (Integrity Measurement Architecture)

IMA measures executable code and libraries.
Stores measurements in TPM PCRs (Platform Configuration Registers).

### 3.2 Measurement flow

1. file read (exec, library load)
2. hash computed
3. hash extended into PCR in TPM
4. PCR becomes cumulative record

### 3.3 Attestation

Local attestation: verify PCR values match expected.
Remote attestation: quote PCRs and send to verifier.

---

## 4. Hardware-Software Interaction

Measurement scenario:
1. boot kernel measured and PCR extended
2. rootfs loaded, measurements accumulated
3. application launched, binary measured
4. TPM holds cumulative integrity record
5. external verifier can confirm system hasn't been modified

---

## 5. Interview Q and A

Q1: Why measure in addition to encrypting (in confidential compute)?
Measurement detects unauthorized modifications; encryption only prevents reading.

Q2: What is PCR in TPM?
Platform Configuration Register; cumulative hash extended as system boots.

Q3: Can you measure user-space applications?
Yes, via IMA and extended attributes.

Q4: How does remote attestation prevent replay?
Timestamp or nonce in attestation request; prevents old quotes from being reused.

Q5: What is UEFI secure boot?
Verification of boot chain via firmware certificates; complementary to runtime IMA.

Q6: How do you recover if measurement doesn't match?
Depends on policy: can deny access, log event, or allow with warning.

---

## 6. Pitfalls and Gotchas

- Assuming measurement prevents attacks (only detects them).
- Confusing measurement with encryption (different concerns).
- Relying on TPM without secure boot (chain of trust incomplete).
- Not including all executable code in measurement (gaps in coverage).

---

## 7. Quick Reference Table

| Component | Purpose |
|---|---|
| IMA | measure executable code and libraries |
| TPM | secure measurement storage |
| PCR | cumulative hash register in TPM |
| attestation | prove system state to external verifier |

| Event | Measurement |
|---|---|
| kernel load | hash extended to PCR 0 |
| initramfs | hash extended to PCR 1 |
| application exec | hash recorded in IMA log |
