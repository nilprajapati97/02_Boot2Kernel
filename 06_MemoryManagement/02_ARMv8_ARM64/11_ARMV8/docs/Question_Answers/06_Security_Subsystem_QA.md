# Security Subsystem — Questions & Answers

---

## Q1. [L1] What is ARM TrustZone? Explain the Secure and Non-Secure worlds.

**Answer:**

```
TrustZone is ARM's hardware security technology that splits the
system into two isolated worlds:

  ┌─────────────────────────────────────────────────────────────┐
  │                    ARMv8 Security States                     │
  │                                                              │
  │    Non-Secure World              Secure World                │
  │   ┌──────────────────┐    ┌──────────────────────┐          │
  │   │  NS-EL0           │    │  S-EL0                │         │
  │   │  User apps         │    │  Trusted Apps (TA)   │         │
  │   │  (Android, Linux)  │    │  (DRM, payment, key) │         │
  │   ├──────────────────┤    ├──────────────────────┤          │
  │   │  NS-EL1           │    │  S-EL1                │         │
  │   │  Linux Kernel      │    │  Secure OS           │         │
  │   │                    │    │  (OP-TEE, Trusty)    │         │
  │   ├──────────────────┤    └──────────────────────┘          │
  │   │  NS-EL2           │                                      │
  │   │  Hypervisor        │                                      │
  │   └──────────────────┘                                       │
  │                                                              │
  │   ┌──────────────────────────────────────────────┐          │
  │   │  EL3 — Secure Monitor                        │          │
  │   │  ARM Trusted Firmware (TF-A / BL31)          │          │
  │   │  • World switching via SMC                   │          │
  │   │  • Manages SCR_EL3.NS bit                    │          │
  │   │  • PSCI power management                     │          │
  │   └──────────────────────────────────────────────┘          │
  └─────────────────────────────────────────────────────────────┘

Hardware enforcement:
  1. SCR_EL3.NS bit: 0=Secure, 1=Non-Secure
     Only EL3 can change this bit
  
  2. Bus-level security: NS bit propagated on AMBA bus
     Memory controller: blocks NS access to Secure memory
     Peripherals: TZPC/TZASC protects Secure devices
  
  3. Memory isolation:
     TZASC (TrustZone Address Space Controller):
       Configures memory regions as Secure or Non-Secure
       Non-Secure accesses to Secure regions → bus error
     
     TZMA (TrustZone Memory Adapter):
       Splits on-chip SRAM into Secure/Non-Secure regions
  
  4. Interrupt isolation:
     GIC Group 0 / Secure Group 1 → Secure interrupts
     Non-Secure Group 1 → Normal World interrupts
     SCR_EL3.FIQ=1 → FIQ always routes to EL3

World switch via SMC (Secure Monitor Call):
  Normal World                 Secure World
  ────────────                 ──────────────
  Linux kernel
    SMC #0 (with function ID)
       │
       ├──→ EL3 Secure Monitor
       │      Save NS context
       │      SCR_EL3.NS = 0
       │      Restore Secure context
       │      ERET to S-EL1
       │                           OP-TEE
       │                           Process request
       │                           SMC return
       │      Save Secure context
       │      SCR_EL3.NS = 1
       │      Restore NS context
       │      ERET to NS-EL1
       │
  Linux continues
```

---

## Q2. [L2] What is SCR_EL3 and what critical security configurations does it control?

**Answer:**

```
SCR_EL3 (Secure Configuration Register) at EL3 controls
fundamental security behavior of the system.

Key bit fields:
┌──────┬──────┬──────────────────────────────────────────────────┐
│ Bit  │ Name │ Function                                        │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [0]  │ NS   │ Non-Secure bit                                  │
│      │      │ 0=Secure state, 1=Non-Secure state              │
│      │      │ Controls which world lower ELs run in          │
│      │      │ Only writable at EL3                            │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [1]  │ IRQ  │ IRQ routing to EL3                              │
│      │      │ 1=all IRQs trap to EL3 (unusual)               │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [2]  │ FIQ  │ FIQ routing to EL3                              │
│      │      │ 1=all FIQs trap to EL3 (standard config)       │
│      │      │ Ensures Secure interrupts reach EL3             │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [3]  │ EA   │ External Abort routing                          │
│      │      │ 1=SError/External aborts trap to EL3            │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [5]  │ RES1 │ Reserved                                        │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [7]  │ SMD  │ SMC Disable                                     │
│      │      │ 1=SMC instruction is UNDEFINED at EL1/EL2      │
│      │      │ Can lock out monitor calls from Normal World   │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [8]  │ HCE  │ HVC Enable                                     │
│      │      │ 1=HVC instruction enabled at EL1/EL2           │
│      │      │ 0=HVC is UNDEFINED (no hypervisor support)     │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [10] │ RW   │ Register Width                                  │
│      │      │ 1=EL2 is AArch64, 0=EL2 is AArch32            │
│      │      │ Controls execution state of next lower EL      │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [11] │ ST   │ Secure timer access                             │
│      │      │ 1=CNTPS timer accessible from Secure EL1       │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [12] │ TWI  │ Trap WFI                                        │
│      │      │ 1=WFI at lower ELs traps to EL3               │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [13] │ TWE  │ Trap WFE                                        │
│      │      │ 1=WFE at lower ELs traps to EL3               │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [21] │ FIEN │ FEAT_IESB: Implicit Error Synchronization      │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [25] │ EnSCXT│ FEAT_CSV2: Enable SCXTNUM_ELx access          │
├──────┼──────┼──────────────────────────────────────────────────┤
│ [36] │ HXEn │ FEAT_HPMN0: Extended HPMN support              │
└──────┴──────┴──────────────────────────────────────────────────┘

Typical TF-A configuration:
  MOV X0, #(SCR_RW_BIT | SCR_HCE_BIT | SCR_NS_BIT)
  // RW=1: EL2 is AArch64
  // HCE=1: HVC enabled
  // NS=1: switch to Non-Secure (for Linux boot)
  ORR X0, X0, #SCR_FIQ_BIT    // Route FIQ to EL3
  MSR SCR_EL3, X0
  ERET                         // Enter NS-EL2 (bootloader)
```

---

## Q3. [L2] How does Secure Boot work on ARM platforms? Describe the TF-A boot chain.

**Answer:**

```
Secure Boot establishes a chain of trust from ROM to OS.

ARM Trusted Firmware-A (TF-A) implements the standard boot flow:

  ┌─────────────────────────────────────────────────────────────┐
  │              ARM Trusted Firmware Boot Chain                 │
  │                                                              │
  │  ┌──────────┐  Hard-coded in ROM, implicitly trusted       │
  │  │ BL1      │  AP Trusted ROM                              │
  │  │ (ROM)    │  • First code executed at reset              │
  │  │          │  • Contains Root of Trust Public Key (ROTPK) │
  │  │          │  • Initializes Secure SRAM                   │
  │  │          │  • Loads and verifies BL2                    │
  │  └────┬─────┘                                               │
  │       │ verify signature against ROTPK                      │
  │  ┌────┴─────┐                                               │
  │  │ BL2      │  Trusted Boot Firmware                       │
  │  │ (SRAM)   │  • Runs at EL3 or EL1-S                     │
  │  │          │  • Initializes DDR memory controller         │
  │  │          │  • Loads and verifies BL31, BL32, BL33       │
  │  │          │  • Uses TBBR (Trusted Board Boot Reqs) spec  │
  │  └────┬─────┘                                               │
  │       │                                                     │
  │   ┌───┴────────────────┬────────────────────────┐          │
  │   │                    │                        │          │
  │ ┌─┴──────┐  ┌─────────┴──┐  ┌─────────────────┴──┐      │
  │ │ BL31   │  │ BL32       │  │ BL33               │      │
  │ │ (EL3)  │  │ (S-EL1)    │  │ (NS-EL2/NS-EL1)   │      │
  │ │        │  │            │  │                    │      │
  │ │ Runtime│  │ Secure OS  │  │ Bootloader         │      │
  │ │ Monitor│  │ OP-TEE     │  │ U-Boot/UEFI       │      │
  │ │ PSCI   │  │ Trusted    │  │ → loads Linux      │      │
  │ │ SMC    │  │ Apps       │  │                    │      │
  │ └────────┘  └────────────┘  └───────────┬────────┘      │
  │                                          │                │
  │                              ┌───────────┴─────────┐     │
  │                              │ Linux Kernel         │     │
  │                              │ (NS-EL1)             │     │
  │                              │ → loads userspace    │     │
  │                              └─────────────────────┘     │
  └─────────────────────────────────────────────────────────────┘

Certificate chain (TBBR):
  Root of Trust Key → Trusted Key Certificate
                    → Non-Trusted Key Certificate
  
  Each BL image is signed:
    BL2: signed by ROTPK (verified by BL1)
    BL31: signed by Trusted World Key (verified by BL2)
    BL32: signed by Trusted World Key (verified by BL2)
    BL33: signed by Non-Trusted World Key (verified by BL2)
  
  If ANY signature check fails → boot STOPS.
  Prevents loading tampered firmware or OS.

Anti-rollback:
  NV counter (fused): incremented with each firmware update
  If image NV counter < fused counter → REJECT (prevents downgrade)
```

---

## Q4. [L3] What is ARM Realm Management Extension (RME) / Confidential Compute Architecture (CCA)?

**Answer:**

```
RME (ARMv9) extends TrustZone from 2 worlds to 4 worlds:

  ┌─────────────────────────────────────────────────────────────┐
  │               ARMv9 RME — Four Security States              │
  │                                                              │
  │  ┌────────────┐ ┌────────────┐ ┌────────────┐              │
  │  │ Non-Secure │ │ Secure      │ │ Realm       │             │
  │  │ World      │ │ World       │ │ World       │             │
  │  │            │ │             │ │             │             │
  │  │ NS-EL0:   │ │ S-EL0:     │ │ R-EL0:     │             │
  │  │  Normal    │ │  Trusted   │ │  Realm     │             │
  │  │  Apps      │ │  Apps (TA) │ │  Apps      │             │
  │  │            │ │             │ │             │             │
  │  │ NS-EL1:   │ │ S-EL1:     │ │ R-EL1:     │             │
  │  │  Linux     │ │  OP-TEE    │ │  Realm OS  │             │
  │  │            │ │             │ │  (Guest VM)│             │
  │  │ NS-EL2:   │ │ S-EL2:     │ │ R-EL2:     │             │
  │  │  KVM       │ │  S-EL2 Mgr│ │  RMM       │             │
  │  └────────────┘ └────────────┘ └────────────┘              │
  │                                                              │
  │  ┌──────────────────────────────────────────────────┐       │
  │  │  Root World (EL3): Monitor firmware               │       │
  │  │  Controls all world transitions                   │       │
  │  └──────────────────────────────────────────────────┘       │
  └─────────────────────────────────────────────────────────────┘

Why Realms?
  TrustZone problem: Secure World is trusted by everyone
    → If OP-TEE is compromised, ALL secrets exposed
    → Cloud VM can't trust the host hypervisor
  
  CCA solution: Realm VMs are protected from EVERYONE:
    → Hypervisor (NS-EL2) can't read Realm memory
    → Secure World can't read Realm memory
    → Only the Realm Management Monitor (RMM) at R-EL2 can access
    → RMM is minimal, attestable, verifiable

Granule Protection Table (GPT):
  New hardware table in physical address space:
  ┌─────────────────────────────────────────────────────┐
  │ Each 4KB physical page (granule) has an owner:      │
  │   00 = Non-Secure                                   │
  │   01 = Secure                                       │
  │   10 = Realm                                        │
  │   11 = Root                                         │
  │                                                     │
  │ GPT checked on EVERY memory access:                 │
  │   NS code accesses Realm page → Granule Protection │
  │   Fault (GPF) → access blocked!                    │
  │                                                     │
  │ Only Root (EL3) can modify GPT entries              │
  └─────────────────────────────────────────────────────┘

Attestation:
  Realm can prove its identity to remote verifier:
  1. Realm: request attestation token from RMM
  2. RMM: measures Realm (hash of code, data, config)
  3. RMM: signs measurement with platform attestation key
  4. Remote verifier: checks signature against expected values
  → Proves Realm hasn't been tampered with
  → Cloud customer can verify their VM is running expected code
```

---

## Q5. [L2] What is Pointer Authentication Code (PAC)? How does it prevent ROP/JOP attacks?

**Answer:**

```
PAC (ARMv8.3) adds cryptographic signatures to pointers,
preventing Return-Oriented Programming (ROP) attacks.

Problem — ROP attack:
  1. Attacker overflows stack buffer
  2. Overwrites return address (LR saved on stack)
  3. Return address now points to attacker's "gadget chain"
  4. Each gadget: small code sequence ending in RET
  5. Chained gadgets = arbitrary code execution

PAC solution:
  ┌──────────────────────────────────────────────────────┐
  │ 64-bit pointer with PAC:                             │
  │                                                      │
  │ ┌───────────┬──────────────────────────────────────┐ │
  │ │ PAC bits  │        Virtual Address                │ │
  │ │ [63:VA_SZ]│        [VA_SZ-1:0]                   │ │
  │ └───────────┴──────────────────────────────────────┘ │
  │                                                      │
  │ With 48-bit VA → 16 bits for PAC (bits [63:48])     │
  │ With 52-bit VA → 12 bits for PAC                    │
  │ With TBI → 7 bits for PAC (top byte ignored)        │
  │                                                      │
  │ PAC = truncated QARMA cipher output:                 │
  │   PAC = QARMA(pointer_value, modifier, key)          │
  │   Key: stored in system register                     │
  │   Modifier: context (SP, or fixed value)             │
  └──────────────────────────────────────────────────────┘

Instructions:
  PACIA X30, SP    // Sign LR with key A, context = SP
    → Computes PAC using X30, SP, and Key A
    → Embeds PAC in upper bits of X30
  
  AUTIA X30, SP    // Verify and strip PAC
    → Recomputes PAC, compares with embedded PAC
    → If match: strips PAC, restores original pointer
    → If MISMATCH: corrupts pointer → SEGFAULT on use!
  
  RETAA            // Combined: AUTIA + RET
    → Authenticate LR and return in one instruction

5 key pairs (each key is 128-bit):
  APIAKey (A): instruction (return addresses)
  APIBKey (B): instruction (alternate)
  APDAKey:     data pointers
  APDBKey:     data pointers (alternate)
  APGAKey:     generic authentication (PACGA)

Function prologue/epilogue with PAC:
  // Prologue:
  PACIASP                    // Sign LR with Key A, SP as modifier
  STP X29, X30, [SP, #-16]! // Save FP and signed LR
  MOV X29, SP
  
  // ... function body ...
  
  // Epilogue:
  LDP X29, X30, [SP], #16   // Restore FP and signed LR
  AUTIASP                    // Verify PAC (crash if tampered!)
  RET

ROP now impossible:
  Attacker overwrites X30 on stack with gadget address
  AUTIASP: PAC verification fails (wrong PAC bits)
  Pointer corrupted → invalid address → SEGFAULT!
```

---

## Q6. [L2] What is Memory Tagging Extension (MTE)? How does it detect memory safety bugs?

**Answer:**

```
MTE (ARMv8.5) assigns 4-bit tags to memory and pointers.
Detects use-after-free, buffer overflow, and other memory bugs.

Concept:
  ┌──────────────────────────────────────────────────────────┐
  │ Memory: each 16-byte aligned granule has a 4-bit tag    │
  │                                                          │
  │ Address     │ Memory contents    │ Tag (stored apart)   │
  │ 0x1000      │ [data............] │ Tag = 0x5           │
  │ 0x1010      │ [data............] │ Tag = 0x3           │
  │ 0x1020      │ [data............] │ Tag = 0x5           │
  │                                                          │
  │ Pointer: bits [59:56] carry a 4-bit "logical tag"       │
  │                                                          │
  │ ┌──────┬────┬──────────────────────────────────────┐    │
  │ │ PAC  │Tag │       Virtual Address [55:0]          │    │
  │ │[63:60]│[59:56]│                                   │    │
  │ └──────┴────┴──────────────────────────────────────┘    │
  │                                                          │
  │ On memory access:                                        │
  │   Logical tag (pointer[59:56]) == Allocation tag?       │
  │   YES → access proceeds                                 │
  │   NO  → TAG MISMATCH FAULT!                             │
  └──────────────────────────────────────────────────────────┘

Detection examples:

  Use-after-free:
    malloc(32)    → ptr = 0x0500_0000_1000, tag = 5
                    Memory at 0x1000: tag set to 5
    free(ptr)     → tag at 0x1000 changed to 7 (random new tag)
    *ptr          → pointer tag = 5, memory tag = 7 → MISMATCH!

  Buffer overflow:
    char *buf = malloc(16);  → tag = 3, memory 0x2000 tag = 3
    buf[16] = 'x';           → address 0x2010, tag = 3
                              → memory 0x2010 tag = 9 (next alloc)
                              → 3 ≠ 9 → MISMATCH!

MTE instructions:
  IRG Xd, Xn, Xm      // Insert Random tag into Xd
  STG Xt, [Xn, #imm]   // Store Allocation Tag to memory
  LDG Xt, [Xn, #imm]   // Load Allocation Tag from memory
  ADDG Xd, Xn, #imm, #tag  // Add and tag
  SUBG                  // Subtract and tag
  STGP Xt1, Xt2, [Xn]  // Store tag pair
  ST2G                  // Store two granule tags
  
  STZGM                 // Store tag and zero (for page clearing)

MTE modes (SCTLR_EL1.TCF):
  00 = No checking (MTE disabled)
  01 = Synchronous: precise fault on mismatch (debug mode)
       → Exact instruction causing mismatch identified
       → Slow (~5-10% overhead)
  10 = Asynchronous: deferred fault reporting (production)
       → TFSRE0_EL1 accumulates tag check failures
       → Checked periodically by kernel
       → <3% overhead, less precise
  11 = Asymmetric: sync for reads, async for writes

Linux support:
  prctl(PR_SET_TAGGED_ADDR_CTRL, ...);
  Android: MTE enabled by default on Pixel 8+ (async mode)
```

---

## Q7. [L3] How does ARM Crypto Extensions (ACE) accelerate cryptographic operations?

**Answer:**

```
ARM Crypto Extensions add dedicated instructions for AES, SHA,
and polynomial multiply, providing hardware-accelerated crypto.

  ┌─────────────────────────────────────────────────────────────┐
  │  Crypto Extension Instructions (using NEON V registers)    │
  │                                                             │
  │  AES:                                                       │
  │    AESE Vd.16B, Vn.16B    // AES single round encrypt     │
  │    AESD Vd.16B, Vn.16B    // AES single round decrypt     │
  │    AESMC Vd.16B, Vn.16B   // AES mix columns              │
  │    AESIMC Vd.16B, Vn.16B  // AES inverse mix columns      │
  │                                                             │
  │    AES-128 round = AESE + AESMC (pipeline-friendly)        │
  │    10 rounds for AES-128, 14 for AES-256                   │
  │    ~1 cycle per byte throughput (vs ~15 cycles SW)          │
  │                                                             │
  │  SHA-1:                                                     │
  │    SHA1C Qd, Sn, Vm.4S    // SHA1 choose                  │
  │    SHA1M Qd, Sn, Vm.4S    // SHA1 majority                │
  │    SHA1P Qd, Sn, Vm.4S    // SHA1 parity                  │
  │    SHA1H Sd, Sn            // SHA1 fixed rotate            │
  │    SHA1SU0, SHA1SU1        // SHA1 schedule update         │
  │                                                             │
  │  SHA-2 (256):                                               │
  │    SHA256H, SHA256H2       // SHA256 hash update           │
  │    SHA256SU0, SHA256SU1    // SHA256 schedule update       │
  │                                                             │
  │  SHA-2 (512, ARMv8.2):                                     │
  │    SHA512H, SHA512H2, SHA512SU0, SHA512SU1                │
  │                                                             │
  │  SHA-3 (ARMv8.2):                                          │
  │    EOR3, RAX1, XAR, BCAX  // SHA3 primitives              │
  │                                                             │
  │  SM3/SM4 (Chinese national crypto, ARMv8.2):               │
  │    SM3SS1, SM3TT1A/B, SM3TT2A/B, SM3PARTW1/W2             │
  │    SM4E, SM4EKEY                                           │
  │                                                             │
  │  PMULL:                                                     │
  │    PMULL Vd.1Q, Vn.1D, Vm.1D  // Polynomial multiply      │
  │    Used for: GCM (Galois Counter Mode) → AES-GCM          │
  │    Carrier-less multiplication for CRC, GHASH              │
  └─────────────────────────────────────────────────────────────┘

Performance comparison (AES-256-GCM, per core):
  Software only:           ~500 MB/s
  With AESE/AESD:          ~3 GB/s
  With AESE + PMULL (GCM): ~5 GB/s
  
  → 10x speedup for TLS/IPsec encryption!

Linux kernel:
  arch/arm64/crypto/
    aes-ce-core.S        // AES using crypto extensions
    sha256-ce-core.S     // SHA-256 using crypto extensions
    ghash-ce-core.S      // GHASH using PMULL
  
  Feature detection: ID_AA64ISAR0_EL1
    AES field:  0x1 = AESE/AESD, 0x2 = + PMULL
    SHA1 field: 0x1 = SHA1C/M/P/H/SU0/SU1
    SHA2 field: 0x1 = SHA256H/H2/SU0/SU1
                0x2 = + SHA512H/H2/SU0/SU1
```

---

## Q8. [L2] How does the SMC (Secure Monitor Call) calling convention work?

**Answer:**

```
SMC Calling Convention (SMCCC) defines the register interface
between Normal World and Secure Monitor.

SMCCC register usage:
  ┌──────┬──────────────────────────────────────────────────────┐
  │ Reg  │ Purpose                                              │
  ├──────┼──────────────────────────────────────────────────────┤
  │ W0   │ Function ID (determines service and function)       │
  │      │ Bit[31]: 0=Fast Call (blocks), 1=Yielding Call      │
  │      │ Bit[30]: 0=SMC32, 1=SMC64                           │
  │      │ Bit[29:24]: Service (0=ARM Arch, 4=OEM, 5=Std)    │
  │      │ Bit[15:0]: Function number                          │
  ├──────┼──────────────────────────────────────────────────────┤
  │ X1-X7│ Arguments (up to 7)                                 │
  ├──────┼──────────────────────────────────────────────────────┤
  │ X0-X3│ Return values (up to 4)                             │
  └──────┴──────────────────────────────────────────────────────┘

Function ID format:
  Bit [31]   : Call type (0=Fast, 1=Yielding)
  Bit [30]   : Convention (0=SMC32, 1=SMC64)
  Bits [29:24]: Service range:
    0x00 = ARM Architecture Calls (PSCI, SMCCC version)
    0x01 = CPU Service
    0x02 = SiP (Silicon Provider) Service
    0x03 = OEM Service
    0x04 = Standard Secure Service (TRNG, etc.)
    0x05 = Standard Hypervisor Service
    0x30-0x31 = Trusted App calls (to OP-TEE TAs)

Common PSCI calls (function IDs):
  PSCI_CPU_ON:       0xC4000003 (SMC64, ARM Arch, func 3)
  PSCI_CPU_OFF:      0x84000002 (SMC32, ARM Arch, func 2)
  PSCI_SYSTEM_RESET: 0x84000009
  PSCI_SYSTEM_OFF:   0x84000008
  PSCI_CPU_SUSPEND:  0xC4000001

Example — PSCI CPU_ON:
  // Linux wants to bring up Core 2 from powered-off state
  
  MOV X0, #0xC4000003   // PSCI_CPU_ON (SMC64)
  MOV X1, #0x0002       // Target CPU MPIDR (Aff0=2)
  LDR X2, =secondary_entry  // Entry point address
  MOV X3, #0            // Context ID (arg to entry point)
  SMC #0                // Call to EL3
  
  // EL3 (TF-A BL31):
  //   1. Power on Core 2's power domain
  //   2. Set Core 2's reset vector to X2
  //   3. Release Core 2 from reset
  //   4. Return success (X0 = 0) to caller
  
  // On X0 return:
  //   0 = SUCCESS
  //  -1 = NOT_SUPPORTED
  //  -2 = INVALID_PARAMETERS
  //  -4 = ALREADY_ON
  //  -6 = ON_PENDING

OP-TEE calls:
  // Linux TEE driver → OP-TEE
  MOV X0, #0x32000001   // OPTEE_SMC_CALL_WITH_ARG
  MOV X1, page_addr     // Shared memory with TA arguments
  SMC #0
  // EL3 switches to Secure World
  // OP-TEE processes request (decrypt key, verify biometric, etc.)
  // EL3 switches back to Normal World
  // X0 = result
```

---

## Q9. [L2] What security features does ARMv8 provide against speculative execution attacks?

**Answer:**

```
ARM implemented multiple mitigations against Spectre-class attacks:

Spectre Variant 1 (Bounds Check Bypass):
  Problem: speculative load bypasses array bounds check
  Mitigation:
    CSDB (Consumption of Speculative Data Barrier):
      Ensures data from speculative loads can't be used
      in subsequent operations (blocks speculative forwarding)
    
    Code pattern:
      CMP X0, X1           // bounds check
      B.HS out_of_bounds    // branch if >= (speculatively skipped)
      CSDB                  // barrier: speculatively loaded data
                            // cannot feed into cache-timing attack
      LDR X2, [X3, X0, LSL#3]  // safe: CSDB prevents misuse

Spectre Variant 2 (Branch Target Injection):
  Problem: attacker poisons branch predictor, causes speculative
           execution of attacker-chosen gadgets
  Mitigations:
    1. CSV2 (FEAT_CSV2): hardware prevents cross-context
       branch predictor poisoning (SCXTNUM_ELx register)
    
    2. SSBS (Speculative Store Bypass Safe):
       PSTATE.SSBS bit: when set, prevents speculative store bypass
       MSR SSBS, #1      // Enable safe mode
    
    3. Branch predictor invalidation:
       Upon context switch: invalidate branch predictor state
       Prevents one process from poisoning another's predictions

Spectre Variant 4 (Speculative Store Bypass):
  Problem: speculative load reads stale data (before store completes)
  Mitigation:
    SSBS (Speculative Store Bypass Safe):
    ID_AA64PFR1_EL1.SSBS field:
      0 = not implemented
      1 = PSTATE.SSBS supported
      2 = MSR SSBS instructions supported

Meltdown:
  Most ARM implementations NOT vulnerable:
    ARM cores use precise permission checking before speculative access
    Only Cortex-A75 was affected (fixed in later revisions)
    Mitigation: KPTI (Kernel Page Table Isolation) if needed

ARM feature registers for speculative attack mitigations:
  ID_AA64PFR0_EL1.CSV2: 0x1 = hardware Spectre v2 mitigation
  ID_AA64PFR0_EL1.CSV3: 0x1 = hardware Spectre v3 safe
  ID_AA64PFR1_EL1.SSBS: Spectre v4 mitigation
  ID_AA64PFR1_EL1.BT:   Branch Target Identification (BTI)

BTI (Branch Target Identification, ARMv8.5):
  Prevents JOP (Jump-Oriented Programming):
  BTI instruction: marks valid branch targets
  
  SCTLR_EL1.BT: enable BTI enforcement
  PTE.GP bit: marks page as "guarded"
  
  If branch lands on non-BTI instruction in guarded page:
  → Branch Target Exception!
  
  Compiler: -mbranch-protection=standard (GCC/Clang)
  Inserts BTI at function entry and PLT stubs.
```

---

## Q10. [L2] How does ARM Secure EL2 (S-EL2) work and why was it added?

**Answer:**

```
S-EL2 (introduced in ARMv8.4) adds a hypervisor level in
the Secure World.

Before S-EL2 (ARMv8.0-8.3):
  ┌──────────────────────────────────────────────────────────┐
  │  Secure World had NO hypervisor:                        │
  │    EL3: Secure Monitor (TF-A)                           │
  │    S-EL1: Secure OS (OP-TEE)                           │
  │    S-EL0: Trusted Applications                          │
  │                                                          │
  │  Problem: only ONE Secure OS can run!                   │
  │    If you need OP-TEE + Hafnium SPM = can't!            │
  │    No isolation between Trusted Applications            │
  │    S-EL1 is monolithic, large trusted computing base    │
  └──────────────────────────────────────────────────────────┘

With S-EL2 (ARMv8.4+):
  ┌──────────────────────────────────────────────────────────┐
  │  Secure World now has its own hypervisor:               │
  │    EL3: Secure Monitor (TF-A)                           │
  │    S-EL2: Secure Partition Manager (Hafnium)            │
  │    S-EL1: multiple Secure Partitions (SPs)             │
  │    S-EL0: per-partition apps                            │
  │                                                          │
  │  ┌──────────────────────────────────────────┐           │
  │  │  S-EL2: SPM (Hafnium)                   │           │
  │  │    Manages multiple secure partitions    │           │
  │  │    Stage-2 translation for SPs           │           │
  │  │    FF-A (Firmware Framework for ARM)      │           │
  │  ├──────────┬──────────┬────────────┤       │           │
  │  │  SP #1   │  SP #2   │  SP #3     │       │           │
  │  │  OP-TEE  │  Crypto  │  DRM       │       │           │
  │  │  (S-EL1) │  (S-EL0) │  (S-EL0)  │       │           │
  │  └──────────┴──────────┴────────────┘       │           │
  └──────────────────────────────────────────────────────────┘

FF-A (Firmware Framework for Arm):
  Standard interface between:
    Normal World ↔ SPM ↔ Secure Partitions
    
  FF-A messages:
    FFA_MSG_SEND_DIRECT_REQ: Normal → SP (synchronous call)
    FFA_MSG_SEND_DIRECT_RESP: SP → Normal (response)
    FFA_MEM_SHARE: share memory between worlds
    FFA_MEM_LEND: lend memory to SP
    
  Replaces ad-hoc SMC-based interfaces with standard protocol.

Benefits:
  1. Multiple SPs: each SP isolated by Stage-2 translation
  2. Smaller TCB: each SP has minimum privileges
  3. Standard interface: FF-A instead of custom SMCs
  4. Compartmentalization: compromised SP can't access others
```

---

## Q11. [L3] What is FEAT_PAuth2 and FEAT_FPAC? How do they improve Pointer Authentication?

**Answer:**

```
Enhanced PAC features improve security of pointer authentication.

FEAT_PAuth2 (ARMv8.6):
  Enhanced PAC algorithm with better cryptographic properties.
  
  Original PAC:
    Uses QARMA block cipher (or IMPLEMENTATION DEFINED)
    Authentication: check PAC bits match expected value
    On failure: corrupt the pointer (set error bit)
    → Process uses corrupted pointer → eventual SEGFAULT
    → But: there's a window between auth and use!
  
  PAuth2 improvements:
    More key bits used in computation
    Better resistance to PAC forgery attacks
    Reduced chance of PAC collision (brute force harder)

FEAT_FPAC (Faulting PAC, ARMv8.6):
  On authentication failure: IMMEDIATE fault instead of
  corrupting the pointer.
  
  Without FPAC:
    AUTIA X30, SP
    // If PAC wrong: X30 = corrupted (error bit set)
    // If attacker catches fault, can retry with different PAC
    // Brute force: try all 2^16 PAC values = 65536 attempts
    // At 1GHz: ~65 microseconds to crack!
    
  With FPAC:
    AUTIA X30, SP
    // If PAC wrong: IMMEDIATE EXCEPTION (no corrupted pointer)
    // Attacker gets ONE attempt before process is killed
    // No brute force possible!
    
  This closes the "PAC oracle" attack:
    Without FPAC: attacker can distinguish "correct PAC" from
    "wrong PAC" by observing crash location
    With FPAC: always faults at authentication instruction

Combined with FEAT_CONSTPACFIELD (ARMv8.6):
  PAC field is always in fixed position regardless of VA size
  Prevents attacks that manipulate VA size to shrink PAC field.

Linux kernel support:
  CONFIG_ARM64_PTR_AUTH=y
  CONFIG_ARM64_PTR_AUTH_KERNEL=y
  
  // Check capability:
  if (system_has_full_ptr_auth()) {
      // Use PAC-protected function pointers
  }
  
  Per-process keys: kernel assigns unique PAC keys per task
  On context switch: load task's keys into APxAKey/APxBKey regs
```

---

## Q12. [L2] How does TZASC (TrustZone Address Space Controller) protect memory regions?

**Answer:**

```
TZASC partitions physical memory into Secure and Non-Secure
regions, enforced at the memory controller level.

Architecture:
  ┌──────────────────────────────────────────────────────────┐
  │                    TZASC (TZC-400)                       │
  │                                                          │
  │  CPU/DMA ──→ [TZASC] ──→ DDR Memory Controller         │
  │                  │                                       │
  │             Access check:                                │
  │             NS bit + Address + Permissions               │
  │             → Allow or DECERR (bus error)                │
  │                                                          │
  │  Up to 9 regions (0-8):                                 │
  │  ┌────────────────────────────────────────────────────┐  │
  │  │ Region │ Start      │ End        │ Security       │  │
  │  ├────────┼────────────┼────────────┼────────────────┤  │
  │  │ 0      │ 0x00000000 │ 0xFFFFFFFF │ NS-RW (default)│  │
  │  │ 1      │ 0x80000000 │ 0x81FFFFFF │ Secure only    │  │
  │  │        │            │            │ (32MB for TEE) │  │
  │  │ 2      │ 0x8E000000 │ 0x8E0FFFFF │ Secure only    │  │
  │  │        │            │            │ (1MB shared)   │  │
  │  │ ...    │ ...        │ ...        │ ...            │  │
  │  └────────────────────────────────────────────────────┘  │
  │                                                          │
  │  Each region has:                                        │
  │    BASE_LOW/HIGH: start address                          │
  │    TOP_LOW/HIGH: end address                             │
  │    ATTR: {s_rd, s_wr, ns_rd, ns_wr} permissions        │
  │    SUBREGION_DISABLE: fine-grained sub-region control   │
  │    FILTER_ENABLE: per-interface filtering                │
  │                                                          │
  │  Higher-numbered regions have HIGHER priority.           │
  │  Overlapping regions: highest-numbered wins.             │
  └──────────────────────────────────────────────────────────┘

Access check:
  1. Transaction arrives: (address, NS bit, read/write)
  2. TZASC finds highest-priority matching region
  3. Checks NS bit against region security:
     - NS transaction to Secure region → DECERR (blocked!)
     - NS transaction to NS region → allowed
     - Secure transaction to any region → generally allowed
  4. Checks R/W permission bits

TZC-400 programming (by TF-A at boot):
  // Configure region 1: Secure memory for OP-TEE
  TZASC_REGION_BASE_LOW(1) = 0x80000000;
  TZASC_REGION_BASE_HIGH(1) = 0x00;
  TZASC_REGION_TOP_LOW(1) = 0x81FFFFFF;
  TZASC_REGION_TOP_HIGH(1) = 0x00;
  TZASC_REGION_ATTR(1) = SECURE_RW | REGION_ENABLE;
  
  // After configuration: lock TZASC (prevent NS modification)
  TZASC_ACTION = DECERR;    // What to do on violation
  TZASC_GATE_KEEPER = 0xF;  // Enable all filter interfaces
  
  After lock: Non-Secure software CANNOT modify TZASC registers.
  Only power cycle or Secure-World can reconfigure.
```

---

Back to [Question & Answers Index](./README.md)
