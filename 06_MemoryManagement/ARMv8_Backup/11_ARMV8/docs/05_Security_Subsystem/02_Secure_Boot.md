# Secure Boot Chain

## 1. What Is Secure Boot?

Secure Boot ensures that only **authenticated, unmodified** software runs on the system. Each stage of the boot process verifies the next stage before executing it.

```
Chain of Trust:

  ┌─────────────────────────────────────────────────────────────┐
  │                                                              │
  │  ROM (BL1)  ──verify──►  BL2  ──verify──►  BL31 (EL3)     │
  │  (immutable)              │                  │               │
  │  Root of Trust            │                  │               │
  │                           ├──verify──►  BL32 (Secure OS)   │
  │                           │                                  │
  │                           └──verify──►  BL33 (Normal World)│
  │                                          │                   │
  │                                          ├──► U-Boot        │
  │                                          │                   │
  │                                          └──► Kernel        │
  │                                               │              │
  │                                               └──► rootfs   │
  └─────────────────────────────────────────────────────────────┘

  Each arrow = cryptographic signature verification
  If ANY verification fails → boot HALTS
```

---

## 2. ARM Trusted Firmware (TF-A / ATF)

TF-A is the reference implementation of Secure World firmware for ARMv8-A.

```
Boot stages defined by ARM:

┌──────┬──────────┬──────────────────────────────────────────────────┐
│Stage │ Name     │ Description                                      │
├──────┼──────────┼──────────────────────────────────────────────────┤
│ BL1  │ Boot ROM │ First code that runs. Burned into ROM.           │
│      │          │ Initializes Secure World, loads BL2.             │
│      │          │ Contains root public key (OTP/eFuse).            │
│      │          │ Runs at EL3.                                     │
├──────┼──────────┼──────────────────────────────────────────────────┤
│ BL2  │ Trusted  │ Loaded by BL1 from flash/eMMC.                  │
│      │ Boot     │ Initializes DRAM, loads BL31/BL32/BL33.         │
│      │ Firmware │ Runs at S-EL1 (or EL3 if BL2-at-EL3).          │
├──────┼──────────┼──────────────────────────────────────────────────┤
│ BL31 │ EL3      │ Secure Monitor. Stays resident forever.         │
│      │ Runtime  │ Handles SMC calls, PSCI, world switching.       │
│      │          │ Runs at EL3.                                     │
├──────┼──────────┼──────────────────────────────────────────────────┤
│ BL32 │ Secure   │ Secure OS (OP-TEE, Trusty, Hafnium SPM).       │
│      │ Payload  │ Provides trusted services.                      │
│      │          │ Runs at S-EL1 (or S-EL2 for SPM).              │
├──────┼──────────┼──────────────────────────────────────────────────┤
│ BL33 │ Normal   │ Normal world bootloader (U-Boot, UEFI, GRUB).  │
│      │ World    │ Loads OS kernel.                                │
│      │ Payload  │ Runs at EL2 or EL1.                            │
└──────┴──────────┴──────────────────────────────────────────────────┘
```

---

## 3. Boot Flow (Step by Step)

```
Power-on Reset:
  ┌──────────────────────────────────────────────────────────────────┐
  │                                                                   │
  │  1. Reset vector fetched from ROM (address 0x0 or vendor-defined)│
  │     └─► BL1 starts executing at EL3, Secure state               │
  │                                                                   │
  │  2. BL1: Minimal hardware init                                   │
  │     • Initialize exception vectors                                │
  │     • Enable instruction cache                                   │
  │     • Configure SCTLR_EL3 (stack, alignment checks)             │
  │     • Load BL2 image from flash into Secure SRAM                │
  │     • Verify BL2 signature against root public key (OTP)        │
  │     • Jump to BL2 at S-EL1                                      │
  │                                                                   │
  │  3. BL2: Platform setup                                          │
  │     • Initialize DRAM controller (DDR training)                  │
  │     • Initialize MMU with static page tables                    │
  │     • Load BL31, BL32, BL33 images from storage                │
  │     • Verify each image signature (chain of trust)               │
  │     • Build handoff data structures (entry points)               │
  │     • Pass control to BL31 at EL3                               │
  │                                                                   │
  │  4. BL31: Runtime setup                                          │
  │     • Initialize GIC (interrupt controller)                      │
  │     • Set up PSCI handlers (CPU on/off)                          │
  │     • Install SMC dispatcher                                     │
  │     • Configure SCR_EL3 for Normal World                        │
  │     • Launch BL32 (Secure OS) at S-EL1                          │
  │     • BL32 initializes, returns to BL31                         │
  │     • Switch to Normal World: set NS=1, ERET to BL33           │
  │                                                                   │
  │  5. BL33: Normal world boot                                     │
  │     • U-Boot/UEFI starts at EL2 (or EL1)                       │
  │     • Loads device tree, kernel image                           │
  │     • Verifies kernel signature (optional — Verified Boot)      │
  │     • Jumps to kernel                                            │
  │                                                                   │
  │  6. Kernel:                                                      │
  │     • Sets up page tables, enables MMU                          │
  │     • Initializes drivers                                        │
  │     • Mounts rootfs                                              │
  │     • Launches init/systemd                                      │
  └──────────────────────────────────────────────────────────────────┘
```

---

## 4. Image Authentication

```
Each boot image contains:
  ┌───────────────────────────────────────────────┐
  │  Image Header                                  │
  │  • Image size                                  │
  │  • Load address                                │
  │  • Entry point                                 │
  │  • Flags                                       │
  ├───────────────────────────────────────────────┤
  │  Image Data (code + data)                      │
  ├───────────────────────────────────────────────┤
  │  Certificate Chain                             │
  │  • Content certificate:                        │
  │    - Hash of image data (SHA-256)             │
  │    - Signed by key certificate's key           │
  │  • Key certificate:                            │
  │    - Public key for next stage                 │
  │    - Signed by root key or parent key          │
  │  • Root certificate:                           │
  │    - Self-signed with root private key         │
  │    - Root public key hash matches OTP value    │
  └───────────────────────────────────────────────┘

Verification:
  1. Compute hash of image data
  2. Verify content certificate signature
  3. Check key certificate chain up to root
  4. Compare root public key hash with OTP value
  5. If all match → image is authentic and untampered
  6. If any fail → abort boot

Algorithms used:
  • RSA-2048 or ECDSA-P256 for signatures
  • SHA-256 for image hashing
  • Key stored in OTP/eFuse (one-time programmable)
```

---

## 5. TBBR (Trusted Board Boot Requirements)

```
ARM's TBBR specification defines the certificate chain:

  ┌─────────────────────────────────────────────────────────────┐
  │                    Certificate Tree                          │
  │                                                              │
  │                  ┌──────────────┐                            │
  │                  │  Root of Trust│                            │
  │                  │  (OTP hash)   │                            │
  │                  └──────┬───────┘                            │
  │                         │                                    │
  │              ┌──────────▼──────────┐                        │
  │              │ Trusted Boot FW     │                        │
  │              │ Key Certificate     │                        │
  │              └──────────┬──────────┘                        │
  │                         │                                    │
  │         ┌───────────────┼───────────────┐                   │
  │         │               │               │                   │
  │   ┌─────▼──────┐ ┌─────▼──────┐ ┌─────▼──────┐           │
  │   │BL31 Key    │ │BL32 Key    │ │BL33 Key    │           │
  │   │Certificate │ │Certificate │ │Certificate │           │
  │   └─────┬──────┘ └─────┬──────┘ └─────┬──────┘           │
  │         │               │               │                   │
  │   ┌─────▼──────┐ ┌─────▼──────┐ ┌─────▼──────┐           │
  │   │BL31 Content│ │BL32 Content│ │BL33 Content│           │
  │   │Certificate │ │Certificate │ │Certificate │           │
  │   │(hash of    │ │(hash of    │ │(hash of    │           │
  │   │ BL31 image)│ │ BL32 image)│ │ BL33 image)│           │
  │   └────────────┘ └────────────┘ └────────────┘           │
  └─────────────────────────────────────────────────────────────┘

TF-A cert_create tool generates this chain:
  $ cert_create --rot-key rot_key.pem \
                --trusted-world-key trusted_world.pem \
                --non-trusted-world-key non_trusted_world.pem \
                --bl31 bl31.bin --bl32 bl32.bin --bl33 bl33.bin
```

---

## 6. Measured Boot & Remote Attestation

```
Beyond Secure Boot (which gates execution), Measured Boot records
what was loaded for later verification:

  Boot Stage    │ Measurement
  ──────────────┼────────────────────────
  BL1           │ Hash(BL2) → PCR[0]
  BL2           │ Hash(BL31) → PCR[1]
                │ Hash(BL32) → PCR[2]
                │ Hash(BL33) → PCR[3]
  BL33 (U-Boot)│ Hash(kernel) → PCR[4]
                │ Hash(DTB) → PCR[5]
  
  Where PCR = Platform Configuration Register (in fTPM or dTPM)
  
  PCR extend operation: PCR[n] = Hash(PCR[n] || measurement)
  This creates a hash chain — any change in ANY stage changes all
  subsequent PCR values.

  Remote Attestation:
  1. Server challenges device: "prove your boot state"
  2. Device's TPM signs current PCR values
  3. Server compares against known-good values
  4. If match → device is in trusted state
```

---

## 7. Anti-Rollback Protection

```
Prevents downgrading firmware to older (vulnerable) versions:

  OTP eFuse counter:
  ┌─────────────────────────────────────┐
  │  Fuse bank: 0 0 0 0 1 1 1 1        │
  │            (burned fuses = 1)       │
  │                                     │
  │  Current NV counter = 4             │
  │  (count of 1s)                      │
  │                                     │
  │  Image header contains: version = 4 │
  │  Boot check: image_version >= NV    │
  │  If image_version < NV → REJECT     │
  │                                     │
  │  On update: burn next fuse          │
  │  New NV counter = 5                 │
  │  Old firmware (version 4) now       │
  │  cannot boot                        │
  └─────────────────────────────────────┘

  TF-A: Implements NV counter checks in authentication module
  Android: Uses Verified Boot (AVB) rollback index
```

---

Next: [Cryptographic Extensions →](./03_Crypto_Extensions.md) | Back to [Security Subsystem Overview](./README.md)
