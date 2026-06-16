# Cryptographic Extensions

## 1. Overview

ARMv8 includes optional hardware cryptographic instructions that accelerate common algorithms directly in the CPU pipeline — no separate crypto coprocessor needed.

```
Crypto Extensions in ARMv8:

┌──────────────┬──────────────────────────────────────────────────┐
│ Extension    │ Instructions & Purpose                           │
├──────────────┼──────────────────────────────────────────────────┤
│ AES          │ AESE, AESD, AESMC, AESIMC                       │
│              │ AES encrypt/decrypt round + MixColumns           │
│              │ ~4x faster than software AES                     │
├──────────────┼──────────────────────────────────────────────────┤
│ SHA-1        │ SHA1C, SHA1P, SHA1M, SHA1H, SHA1SU0, SHA1SU1   │
│              │ SHA-1 hash computation (legacy, not recommended) │
├──────────────┼──────────────────────────────────────────────────┤
│ SHA-2 (256)  │ SHA256H, SHA256H2, SHA256SU0, SHA256SU1         │
│              │ SHA-256 hash (TLS, file integrity)               │
├──────────────┼──────────────────────────────────────────────────┤
│ SHA-2 (512)  │ SHA512H, SHA512H2, SHA512SU0, SHA512SU1         │
│ (ARMv8.2)   │ SHA-512 hash                                     │
├──────────────┼──────────────────────────────────────────────────┤
│ SHA-3        │ EOR3, RAX1, XAR, BCAX                           │
│ (ARMv8.2)   │ SHA-3/Keccak primitives                          │
├──────────────┼──────────────────────────────────────────────────┤
│ SM3          │ SM3SS1, SM3TT1A/B, SM3TT2A/B, SM3PARTW1/W2     │
│ (ARMv8.2)   │ Chinese national hash standard                   │
├──────────────┼──────────────────────────────────────────────────┤
│ SM4          │ SM4E, SM4EKEY                                    │
│ (ARMv8.2)   │ Chinese national block cipher                    │
├──────────────┼──────────────────────────────────────────────────┤
│ CRC32       │ CRC32B, CRC32H, CRC32W, CRC32X                  │
│              │ CRC32CB, CRC32CH, CRC32CW, CRC32CX              │
│              │ CRC-32 and CRC-32C checksums                     │
├──────────────┼──────────────────────────────────────────────────┤
│ PMULL       │ PMULL, PMULL2                                     │
│              │ Polynomial multiply for GCM (AES-GCM mode)       │
└──────────────┴──────────────────────────────────────────────────┘

Detection (read ID_AA64ISAR0_EL1):
  Bits [7:4]   = AES (0x1=AESE/D, 0x2=+PMULL)
  Bits [11:8]  = SHA1
  Bits [15:12] = SHA2 (0x1=SHA256, 0x2=+SHA512)
  Bits [35:32] = SHA3
  Bits [43:40] = SM3
  Bits [47:44] = SM4
```

---

## 2. AES Instructions

```
AES-128 uses 10 rounds. Each round consists of:
  SubBytes → ShiftRows → MixColumns → AddRoundKey

ARM provides 4 instructions that map to AES operations:

  AESE  Vd.16B, Vn.16B   // Single AES encrypt round
                           // (SubBytes + ShiftRows + XOR with key)
  
  AESMC Vd.16B, Vn.16B   // AES MixColumns
  
  AESD  Vd.16B, Vn.16B   // Single AES decrypt round
                           // (InvSubBytes + InvShiftRows + XOR)
  
  AESIMC Vd.16B, Vn.16B  // AES Inverse MixColumns

AES-128 encryption (10 rounds):
  // V0 = plaintext, V1-V10 = round keys
  AESE   V0.16B, V1.16B    // Round 1
  AESMC  V0.16B, V0.16B
  AESE   V0.16B, V2.16B    // Round 2
  AESMC  V0.16B, V0.16B
  ...                        // Rounds 3-9 same pattern
  AESE   V0.16B, V10.16B   // Round 10 (no MixColumns)
  EOR    V0.16B, V0.16B, V11.16B  // Final AddRoundKey

Performance comparison (cycles per byte):
  ┌──────────────────┬────────────────┬───────────────┐
  │ Method           │ Cycles/byte    │ Throughput     │
  ├──────────────────┼────────────────┼───────────────┤
  │ Software AES     │ ~15-20         │ ~100 MB/s     │
  │ ARMv8 CE AES     │ ~1-3           │ ~1-3 GB/s    │
  │ (pipelined)      │                │               │
  └──────────────────┴────────────────┴───────────────┘
```

---

## 3. AES-GCM (Galois/Counter Mode)

```
AES-GCM is the most common TLS cipher. It needs:
  1. AES-CTR for encryption
  2. GHASH for authentication (polynomial multiplication in GF(2^128))

ARM provides PMULL for GHASH:

  PMULL  V0.1Q, V1.1D, V2.1D   // Polynomial multiply lower
  PMULL2 V0.1Q, V1.2D, V2.2D   // Polynomial multiply upper

  GHASH computes: X_i = (X_{i-1} XOR C_i) * H  in GF(2^128)
  Where H is the hash key and C_i is ciphertext block i

  AES-GCM loop:
    // Encrypt (AES-CTR):
    AESE   V0.16B, Vkey.16B
    AESMC  V0.16B, V0.16B
    ...
    EOR    V_ct.16B, V_pt.16B, V0.16B  // ciphertext = pt XOR keystream
    
    // Authenticate (GHASH):
    EOR    V_hash.16B, V_hash.16B, V_ct.16B
    PMULL  V_tmp.1Q, V_hash.1D, V_H.1D    // GF multiply
    PMULL2 V_tmp2.1Q, V_hash.2D, V_H.2D   // (Karatsuba)
    // Reduce mod polynomial...
    
  This is why TLS on ARM64 is fast — both AES and GHASH use hardware
```

---

## 4. SHA-256 Instructions

```
SHA-256 processes 64-byte blocks. Each block requires 64 rounds.

ARMv8 SHA instructions process 4 rounds at a time:

  SHA256H   Qd, Qn, Vm.4S   // 4 SHA-256 rounds (working vars a-d)
  SHA256H2  Qd, Qn, Vm.4S   // 4 SHA-256 rounds (working vars e-h)
  SHA256SU0 Vd.4S, Vn.4S    // SHA-256 message schedule (part 1)
  SHA256SU1 Vd.4S, Vn.4S, Vm.4S  // SHA-256 schedule (part 2)

SHA-256 with crypto extensions (simplified):
  // V0-V3 contain the 16-word message schedule (W0-W15)
  // V4 = state {a,b,c,d}, V5 = state {e,f,g,h}
  // V16 = round constants K[0..3]
  
  ADD    V6.4S, V0.4S, V16.4S     // W + K
  SHA256H   Q4, Q5, V6.4S         // 4 rounds on a-d
  SHA256H2  Q5, Q4_orig, V6.4S    // 4 rounds on e-h
  SHA256SU0 V0.4S, V1.4S          // Schedule: next W values
  SHA256SU1 V0.4S, V2.4S, V3.4S   // Schedule: complete
  
  // Repeat 16 times (64 rounds / 4 per iteration)

Performance:
  Software SHA-256: ~15 cycles/byte
  ARMv8 CE SHA-256: ~3-5 cycles/byte
```

---

## 5. CRC32 Instructions

```
CRC32 is used for data integrity (Ethernet, storage, compression):

  CRC32B  Wd, Wn, Wm   // CRC-32 of byte
  CRC32H  Wd, Wn, Wm   // CRC-32 of halfword
  CRC32W  Wd, Wn, Wm   // CRC-32 of word
  CRC32X  Wd, Wn, Xm   // CRC-32 of doubleword

  CRC32CB Wd, Wn, Wm   // CRC-32C (Castagnoli) of byte
  CRC32CH Wd, Wn, Wm   // CRC-32C of halfword
  CRC32CW Wd, Wn, Wm   // CRC-32C of word
  CRC32CX Wd, Wn, Xm   // CRC-32C of doubleword

  CRC-32:  polynomial 0x04C11DB7 (Ethernet, ZIP, PNG)
  CRC-32C: polynomial 0x1EDC6F41 (iSCSI, ext4, btrfs)

Example: compute CRC32C of a buffer
  MOV   W0, #0xFFFFFFFF      // Initial CRC
loop:
  LDR   X1, [X2], #8         // Load 8 bytes
  CRC32CX W0, W0, X1         // Update CRC (8 bytes at a time)
  SUBS  X3, X3, #8
  B.GT  loop
  MVN   W0, W0               // Final complement

  vs software: ~10x faster, single-cycle per 8 bytes
```

---

## 6. Detection & Linux Support

```
Checking for crypto extension support:

Assembly:
  MRS X0, ID_AA64ISAR0_EL1
  // Check bits [7:4] for AES support
  // Check bits [15:12] for SHA2 support

Linux /proc/cpuinfo:
  Features: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics ...

Linux kernel uses crypto extensions automatically:
  • aes-arm64-ce    (AES using crypto extensions)
  • sha256-arm64-ce (SHA-256 using crypto extensions)
  • ghash-arm64-ce  (GHASH for AES-GCM)
  • crc32-arm64-ce  (CRC32 using CE)
  
  Selected automatically when CONFIG_CRYPTO_AES_ARM64_CE=y
  Falls back to software if CE not available

OpenSSL:
  Detects ARMv8 CE at runtime via HWCAP
  Uses aes-arm64-ce for TLS
  Performance: AES-256-GCM ~3 GB/s on Cortex-A76 @ 2.8 GHz
```

---

## 7. Random Number Generation (RNDR)

```
ARMv8.5 adds hardware random number generation:

  RNDR  Xd    // Read 64-bit random number
               // Returns NZCV.Z=1 if entropy exhausted (retry)
  
  RNDRRS Xd   // Read random number, reseeded
               // Higher security — forces reseed from TRNG

Usage:
  loop:
    RNDR  X0           // Try to get random number
    B.EQ  loop         // Retry if entropy exhausted (Z flag set)
    // X0 = 64-bit hardware random number

  Linux: /dev/hwrng uses RNDR if available
  Detection: ID_AA64ISAR0_EL1 bits [63:60]
```

---

Next: Back to [Security Subsystem Overview](./README.md) | Continue to [Virtualization Subsystem →](../06_Virtualization_Subsystem/)
