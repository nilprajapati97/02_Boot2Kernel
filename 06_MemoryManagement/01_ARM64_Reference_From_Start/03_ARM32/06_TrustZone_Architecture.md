# ARM32 TrustZone Architecture
## Document 6: Secure/Non-Secure Worlds, Monitor Mode, TZASC, Secure Boot

**Author:** Senior Kernel Engineer  
**Target:** ARMv7-A (ARM32), TrustZone Security Extensions  
**Scope:** TrustZone hardware, Monitor Mode, TZASC, TZPC, OP-TEE, Qualcomm QSEE, Google Trusty  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Document 01 (Architecture Overview), Document 03 (Linux MM Internals)

---

## Table of Contents
1. [TrustZone Fundamentals](#1-trustzone-fundamentals)
2. [Processor State and World Switching](#2-processor-state-and-world-switching)
3. [Secure Memory Configuration](#3-secure-memory-configuration)
4. [TZASC — TrustZone Address Space Controller](#4-tzasc--trustzone-address-space-controller)
5. [TZPC — TrustZone Protection Controller](#5-tzpc--trustzone-protection-controller)
6. [Secure Page Tables and MMU](#6-secure-page-tables-and-mmu)
7. [Monitor Mode and SMC Handling](#7-monitor-mode-and-smc-handling)
8. [Interrupt Routing and FIQ](#8-interrupt-routing-and-fiq)
9. [OP-TEE Architecture (Open Source TEE)](#9-op-tee-architecture-open-source-tee)
10. [Qualcomm QSEE Architecture](#10-qualcomm-qsee-architecture)
11. [Google Trusty TEE](#11-google-trusty-tee)
12. [Secure Boot and Chain of Trust](#12-secure-boot-and-chain-of-trust)
13. [Security Vulnerabilities and Mitigations](#13-security-vulnerabilities-and-mitigations)

---

## 1. TrustZone Fundamentals

### 1.1 Two World Architecture

```
ARM TrustZone divides the SoC into two isolated worlds:

┌─────────────────────────────────────────────────────────────────┐
│                    NON-SECURE WORLD (HLOS)                      │
│                                                                  │
│   OS: Android / Linux                                           │
│   Applications: User apps, system services                      │
│   Memory: Accessible to all, no hardware protection             │
│   Bus transactions: NS=1 bit on AXI bus                        │
│                                                                  │
│   Can request secure services via SMC instruction               │
└─────────────────────────────────────────────────────────────────┘
                            │ SMC instruction
                            │ (Secure Monitor Call)
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                     MONITOR MODE (PL3)                          │
│                                                                  │
│   Context Save/Restore (banked registers, CP15 state)          │
│   World switch: Non-Secure ↔ Secure                            │
│   Implemented in TF-A (Trusted Firmware-A) BL31 or OEM SMC    │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                    SECURE WORLD (TEE)                           │
│                                                                  │
│   TEE OS: OP-TEE / Qualcomm QSEE / Google Trusty               │
│   Trusted Applications (TAs): DRM, biometrics, key storage     │
│   Memory: Protected by TZASC (inaccessible from Non-Secure)    │
│   Bus transactions: NS=0 bit on AXI bus                        │
│                                                                  │
│   Has full access to Non-Secure memory (for sharing data)      │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 Security State Determination

```
The NS (Non-Secure) bit flows through the system:

1. CPU state: SCR.NS bit (in Monitor/Secure modes)
   SCR (Secure Configuration Register, CP15 c1 c1 0):
   [0] NS  = 0 → Secure world
            = 1 → Non-Secure world (only settable from Secure/Monitor)
   [1] IRQ = 1 → IRQ taken to Monitor mode (default: to current world)
   [2] FIQ = 1 → FIQ taken to Monitor mode
   [3] EA  = 1 → External aborts to Monitor mode
   [4] FW  = 1 → CPSR.F can be modified in Non-Secure state
   [5] AW  = 1 → CPSR.A can be modified in Non-Secure state
   [6] nET = Net: Endianness of exceptions to Monitor mode
   [7] SCD = SMC Disable bit (if 1: SMC from Non-Secure generates UNDEF)
   [8] HCE = Hypervisor Call Enable (if 1: HVC instruction enabled)
   [9] SIF = Secure Instruction Fetch (if 1: Secure state cannot fetch from NS memory)

2. AXI bus: Each transaction carries NS bit
   NS=0 → Secure transaction → can access secure memory
   NS=1 → Non-Secure transaction → blocked from secure memory

3. Memory controller: TZASC enforces NS bit per region
```

### 1.3 Privilege Levels in TrustZone

```
ARM32 Privilege Levels with TrustZone:

           Non-Secure World          Secure World
PL0:  User applications (NS)    Trusted Application user mode (S)
PL1:  Linux Kernel (NS)         TEE OS kernel (S) — e.g., OP-TEE core
PL2:  Hypervisor (NS, Hyp mode) [Shared: Monitor mode is technically PL3 in TZ]
PL3:  [Monitor Mode — handles world switch]

Register banking across worlds:
  Most CP15 registers are BANKED:
    Secure world has its own: TTBR0, TTBR1, DACR, DFSR, DFAR, SCTLR, etc.
    Non-Secure world has its own set
  SCR is ONLY accessible from Monitor or Secure PL1
  VBAR, MVBAR: banked (Secure and Non-Secure exception vectors)
```

---

## 2. Processor State and World Switching

### 2.1 Register Banking

```
Banked registers (Secure vs Non-Secure independent copies):

CP15 registers:
  c1  c0 0: SCTLR           (system control)
  c2  c0 0: TTBR0           (page table base for user)
  c2  c0 1: TTBR1           (page table base for kernel)
  c2  c0 2: TTBCR           (TTBR control)
  c3  c0 0: DACR            (domain access control)
  c5  c0 0: DFSR            (data fault status)
  c5  c0 1: IFSR            (instruction fault status)
  c6  c0 0: DFAR            (data fault address)
  c6  c0 2: IFAR            (instruction fault address)
  c12 c0 0: VBAR            (vector base address)
  c13 c0 0: FCSEIDR
  c13 c0 1: CONTEXTIDR      (ASID + PROCID)
  c13 c0 2: TPIDRURW        (user R/W thread pointer)
  c13 c0 3: TPIDRURO        (user R/O thread pointer)
  c13 c0 4: TPIDRPRW        (privileged thread pointer)

Non-banked (shared, only accessible from Secure):
  c1  c1 0: SCR             (secure configuration)
  c12 c0 1: MVBAR           (monitor vector base address)
  c1  c1 1: SDER
  c10 c2 0: PRRR
  c10 c2 1: NMRR
```

### 2.2 World Switch — Non-Secure to Secure

```assembly
/* Triggered by SMC instruction in Non-Secure PL0 or PL1 */

/* CPU automatically:
 *  1. Sets SCR.NS = 0 (or stays if already in monitor)
 *  2. Changes mode to Monitor (PL3)
 *  3. Saves CPSR to SPSR_mon
 *  4. Sets PC = MVBAR[31:5] + 0x08 (SMC vector in MVBAR)
 */

/* Monitor handler (in TF-A BL31 or OEM code): */
smc_handler:
    /* Save Non-Secure context */
    PUSH {r0-r12, lr}                  @ Save GP registers
    MRC  p15, 0, r0, c2, c0, 0        @ Save NS TTBR0
    STRD r0, [sp, #NS_TTBR0_OFFSET]
    /* ... save all banked NS CP15 registers ... */

    /* Switch to Secure world */
    MRC  p15, 0, r0, c1, c1, 0        @ Read SCR
    BIC  r0, r0, #1                    @ Clear SCR.NS = 0 (Secure)
    MCR  p15, 0, r0, c1, c1, 0        @ Write SCR → now in Secure world
    ISB

    /* Restore Secure context */
    LDRD r0, [sp, #SEC_TTBR0_OFFSET]
    MCR  p15, 0, r0, c2, c0, 0        @ Restore Secure TTBR0
    /* ... restore all banked Secure CP15 registers ... */

    /* Jump to Secure TEE handler */
    LDR  pc, =tee_smc_handler
```

### 2.3 World Switch Latency

```
Context save/restore cost per world switch:
  Save/restore GP registers (r0-r12, lr, sp): ~12 cycles
  Save/restore CP15 banked registers: ~15 registers × 2 cycles = 30 cycles
  TLB and cache management: 20-50 cycles
  Pipeline flush (ISB): ~5 cycles
  Total: ~100-150 cycles per world switch

Optimization strategies:
  - Lazy save of floating-point registers (VFP/NEON): only save if used
  - Minimize world switch frequency (batch requests)
  - Fast path for common TAs (keep them in Secure world continuously)
```

---

## 3. Secure Memory Configuration

### 3.1 Memory Partitioning with TrustZone

```
Physical Memory Map with TrustZone:

0xFFFFFFFF ┌──────────────────┐
           │  Secure World     │ ← Protected by TZASC
           │  TEE OS           │   (e.g., 32MB at top of RAM)
           │  Trusted Apps     │   TZASC blocks NS transactions here
0xFE000000 ├──────────────────┤
           │  Shared Memory    │ ← Accessible by both worlds
           │  (SMC parameters) │   Mapped in both S and NS page tables
0xFC000000 ├──────────────────┤
           │  Non-Secure       │ ← Normal Linux kernel + user memory
           │  Linux / Android  │
0x00000000 └──────────────────┘

The TZASC hardware enforces:
  NS transaction to 0xFE000000–0xFFFFFFFF → ABORT (secure memory access denied)
  S transaction to 0xFE000000–0xFFFFFFFF → ALLOWED
  S transaction to 0x00000000–0xFBFFFFFF → ALLOWED (secure can read NS)
```

### 3.2 NS Bit in Page Descriptors

```
Every ARM page descriptor has an NS bit:

Section descriptor [19]: NS
  NS=0: Physical address is in Secure memory space
  NS=1: Physical address is in Non-Secure memory space

Page table (L1) descriptor [3]: NS (for L2 table itself)
Small page descriptor: No explicit NS bit
  (inherits from L1 table descriptor's NS setting for table walk)

Significance:
  Secure world page table can map:
    - Secure physical memory (NS=0) → TEE OS code/data
    - Non-Secure physical memory (NS=1) → shared buffers
  Non-Secure page table can only map:
    - Non-Secure physical memory
    - Attempt to map NS=0 PA from NS state → ABORT

Example: Secure TEE mapping shared buffer at 0xFC000000:
  L1 descriptor: NS=1 (points to NS physical memory)
  So Secure page table maps NS PA for reading shared data
```

---

## 4. TZASC — TrustZone Address Space Controller

### 4.1 TZASC Architecture (TZC-380 / TZC-400)

```
TZASC (TrustZone Address Space Controller):
  - Sits on the memory bus between AXI interconnect and DRAM controller
  - Inspects every memory transaction's NS bit
  - Configures secure/non-secure regions in DRAM

ARM TZC-400 (found in many modern SoCs):

┌───────────────────────────────────────────────────────┐
│                     AXI Interconnect                  │
│    (CPU, DMA, GPU, all bus masters)                   │
└──────────────────────┬────────────────────────────────┘
                       │ AXI transactions (with NS bit)
                       ▼
┌───────────────────────────────────────────────────────┐
│               ARM TZC-400                             │
│                                                        │
│  Region 0 (background): 0x00000000–0xFFFFFFFF        │
│    Default: NS allowed or blocked                     │
│                                                        │
│  Region 1: 0x80000000–0x81FFFFFF (32MB Secure)       │
│    NS access: BLOCKED (generates AXI error)           │
│    S access:  ALLOWED                                  │
│                                                        │
│  Registers:                                           │
│    TZASC_BUILD_CONFIG  (0x000): Read capabilities    │
│    TZASC_GATE_KEEPER   (0x008): Control              │
│    TZASC_SPECULATION   (0x00C): AXI speculation      │
│    TZASC_REGn_BASE     (0x100+n*32): Region base     │
│    TZASC_REGn_TOP      (0x104+n*32): Region top      │
│    TZASC_REGn_ATTR     (0x108+n*32): NS/S setting    │
└──────────────────────┬────────────────────────────────┘
                       │ Filtered transactions
                       ▼
              ┌────────────────┐
              │  DRAM Controller│
              └────────────────┘
```

### 4.2 TZASC Configuration (ARM Trusted Firmware / BL2)

```c
/* Typical TZASC setup in ARM Trusted Firmware (TF-A) BL2 */
/* File: plat/vendor/bl2_plat_setup.c */

#define TZASC_BASE          0xF8004000UL
#define SECURE_DRAM_BASE    0x7E000000UL    /* 32MB secure at top of 2GB */
#define SECURE_DRAM_SIZE    0x02000000UL    /* 32MB */

void tzasc_configure(void)
{
    /* Disable TZASC before programming (gate keeper) */
    mmio_write_32(TZASC_BASE + TZASC_GATE_KEEPER, 0);

    /* Region 0: Background region — allow all NS access to non-secure RAM */
    mmio_write_32(TZASC_BASE + TZASC_REG0_ATTR,
                  TZC_REGION_S_RDWR | TZC_REGION_NS_RDWR);

    /* Region 1: Secure TEE region — NS access blocked */
    mmio_write_32(TZASC_BASE + TZASC_REG1_BASE, SECURE_DRAM_BASE);
    mmio_write_32(TZASC_BASE + TZASC_REG1_TOP,
                  SECURE_DRAM_BASE + SECURE_DRAM_SIZE - 1);
    mmio_write_32(TZASC_BASE + TZASC_REG1_ATTR,
                  TZC_REGION_S_RDWR |    /* Secure can RW */
                  TZC_REGION_NS_NONE);   /* Non-Secure: BLOCKED */

    /* Enable TZASC */
    mmio_write_32(TZASC_BASE + TZASC_GATE_KEEPER, 1);

    /* From this point: NS access to SECURE_DRAM_BASE → AXI ERROR RESPONSE */
}
```

### 4.3 TZC-400 vs TZC-380 Differences

| Feature | TZC-380 | TZC-400 |
|---------|---------|---------|
| Max Regions | 8 | 9 (region 0 + 8 programmable) |
| Filter ports | Up to 4 | Up to 4 |
| NSAID filtering | No | Yes (filter by NS AXI master ID) |
| Address range | 32-bit | Up to 64-bit |
| Used in | Cortex-A9 era | Cortex-A53/A57+ |

---

## 5. TZPC — TrustZone Protection Controller

### 5.1 TZPC Purpose

```
TZPC (TrustZone Protection Controller):
  - Controls access to PERIPHERALS (not DRAM like TZASC)
  - Makes individual peripherals accessible only from Secure world
  - Typically controls: UART, GPIO, Crypto engine, OTP, Secure Timer

┌───────────────────────────────────────────────────────┐
│               TZPC (e.g., BP141)                      │
│                                                        │
│  TZPCDECPROT0: [31:0] = 32 peripherals, 1 bit each   │
│    bit=0: Peripheral accessible from NS and S         │
│    bit=1: Peripheral accessible from S ONLY           │
│                                                        │
│  TZPCDECPROT1: Next 32 peripherals                    │
│                                                        │
│  Only writable from Secure PL1 (TEE OS)               │
└───────────────────────────────────────────────────────┘

Examples:
  Secure-only peripherals: Hardware crypto engine, OTP fuses,
                            biometric sensor, secure display path
  Non-Secure peripherals:  USB, SDIO, ETH, audio
  Dynamic:                 Touchscreen (Secure during PIN entry)
```

---

## 6. Secure Page Tables and MMU

### 6.1 Secure World MMU Configuration

```
The Secure world has COMPLETELY SEPARATE CP15 MMU registers:

Secure SCTLR:
  Typically: MMU=ON, I$=ON, D$=ON, XN enforced
  Written by TEE OS during initialization

Secure TTBR0/TTBR1/TTBCR:
  Points to Secure page tables in secure memory
  NS Linux cannot read/write these (not accessible from NS)

Secure DACR: Domain configuration for Secure world
Secure VBAR: Exception vectors for Secure world (TEE exception handlers)

Physical layout of Secure page tables:
  Located in TZASC-protected memory
  Mapped with NS=0 in Secure page table descriptors
  TEE OS creates identity-ish mapping for its own memory + shared buffers
```

### 6.2 Secure Mapping Example (OP-TEE)

```
OP-TEE Secure Memory Layout (example, 32MB at 0xFE000000):

0xFFFFFFFF ┌──────────────────┐
           │ Secure Vectors   │ (high vectors, OP-TEE handlers)
0xFFFF0000 ├──────────────────┤
           │ OP-TEE Core      │ Mapped R+X (no write)
           │ (kernel code)    │
0xFF000000 ├──────────────────┤
           │ Trusted App      │ Mapped per-TA (mmap-like)
           │ Heap             │
0xFE800000 ├──────────────────┤
           │ OP-TEE Stack     │ Per-CPU secure stacks
           │ + Shared Memory  │
0xFE000000 └──────────────────┘ ← TZASC boundary

Secure page table (L1, 16KB at 0xFE000000):
  Entry for 0xFF000000: Section, NS=0, R+X, Secure domain
  Entry for 0xFC000000: Page table, NS=1 → L2 for shared NS pages
  (Secure code can map NS memory with NS=1 in descriptor)
```

---

## 7. Monitor Mode and SMC Handling

### 7.1 SMC Calling Convention (SMCCC — SMC Calling Convention)

```
ARM SMC Calling Convention (DEN0028):

SMC instruction passes parameters via registers:
  r0: Function identifier (service type + function number)
      [31]    Fast call (1) vs Yielding call (0)
      [30]    SMC32 (0) vs SMC64 (1, ARM64 only)
      [29:24] Service type:
              0x00 = ARM Architecture calls
              0x01 = CPU Service calls
              0x02 = SiP (SoC-specific)
              0x04 = Standard Service calls (PSCI etc.)
              0x30 = Trusted OS Calls (OP-TEE convention)
      [23:0]  Function number
  
  r1–r3: Input parameters
  r0–r3: Return values (on return from Secure)

Example: OP-TEE Open Session SMC
  r0 = 0x32000002   (OP-TEE Open Session)
  r1 = TA UUID high word
  r2 = TA UUID low word
  r3 = operation parameters pointer
  SMC
  r0 = return code (0 = success)
  r1 = session handle
```

### 7.2 PSCI (Power State Coordination Interface)

```c
/*
 * PSCI: ARM standard SMC interface for power management
 * Implemented in TF-A (BL31) or OEM firmware
 * Linux uses PSCI for CPU hotplug, suspend, system shutdown
 */

/* PSCI function IDs */
#define PSCI_VERSION         0x84000000
#define PSCI_CPU_SUSPEND     0x84000001
#define PSCI_CPU_OFF         0x84000002
#define PSCI_CPU_ON          0x84000003
#define PSCI_AFFINITY_INFO   0x84000004
#define PSCI_SYSTEM_OFF      0x84000008
#define PSCI_SYSTEM_RESET    0x84000009

/* Linux PSCI driver: arch/arm/kernel/psci.c */
int psci_ops_init(void)
{
    /* Detect PSCI via Device Tree: /psci node */
    /* or ACPI FADT */

    psci_ops.cpu_on = psci_cpu_on;
    psci_ops.cpu_off = psci_cpu_off;
    psci_ops.cpu_suspend = psci_cpu_suspend;
}

static int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
    /* Call PSCI via SMC (or HVC if hypervisor) */
    return invoke_psci_fn(PSCI_CPU_ON, cpuid, entry_point, 0);
}

static unsigned long invoke_psci_fn(u32 fn, unsigned long a0,
                                     unsigned long a1, unsigned long a2)
{
    /* Inline assembly: SMC #0 */
    register unsigned long r0 asm("r0") = fn;
    register unsigned long r1 asm("r1") = a0;
    register unsigned long r2 asm("r2") = a1;
    register unsigned long r3 asm("r3") = a2;
    asm volatile("smc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3) : "memory");
    return r0;
}
```

---

## 8. Interrupt Routing and FIQ

### 8.1 FIQ as Secure Interrupt

```
TrustZone Interrupt Routing:

GIC (Generic Interrupt Controller) with TrustZone:
  Each interrupt has a security setting: Secure or Non-Secure

  Secure interrupt → routed as FIQ to CPU
    SCR.FIQ = 1 → FIQ taken to Monitor mode
    Monitor saves NS context, enters Secure world, handles FIQ
    Typical: Secure timer, secure watchdog, crypto completion
  
  Non-Secure interrupt → routed as IRQ to CPU
    Handled by Linux IRQ handler normally

GIC-400 (ARMv7-A era):
  GICD_ISACTIVER, GICD_ISENABLER: Per-interrupt enable
  GICD_IGROUPR: Set bit=0 → Secure, bit=1 → Non-Secure
  Only writable from Secure state
```

### 8.2 FIQ Handler in Monitor

```assembly
/* FIQ taken to Monitor mode (MVBAR + 0x1C) */
/* This is for secure FIQ handling */

secure_fiq_handler:
    /* On entry: SCR.NS may be 0 or 1 depending on which world was running */
    /* Monitor mode: can access both worlds' resources */

    /* Check which world was interrupted */
    MRC p15, 0, r0, c1, c1, 0  @ Read SCR
    TST r0, #SCR_NS             @ Was Non-Secure world running?
    
    BEQ secure_world_fiq        @ Already in Secure world
    
    /* Save Non-Secure FIQ state */
    /* ... context save ... */
    
    /* Switch to Secure world to handle FIQ */
    BIC r0, r0, #SCR_NS
    MCR p15, 0, r0, c1, c1, 0
    ISB

    /* Call Secure FIQ handler in TEE */
    BL tee_handle_fiq

    /* Restore NS world if needed */
    /* ... return to interrupted world ... */
    SUBS pc, lr, #4             @ Return from FIQ
```

---

## 9. OP-TEE Architecture (Open Source TEE)

### 9.1 OP-TEE Software Stack

```
┌──────────────────────────────────────────────────────────────────┐
│                    Non-Secure World (Linux)                       │
│                                                                   │
│  User space:    tee-supplicant  ← daemon for file-based TAs     │
│                 CA (Client App) — uses /dev/tee0                 │
│                 libteec.so      ← GlobalPlatform TEE API         │
│                                                                   │
│  Kernel space:  optee.ko (OP-TEE driver)                        │
│                 /dev/tee0, /dev/teepriv0                         │
│                 Calls SMC to enter Secure world                  │
└──────────────────────────────────────────────────────────────────┘
                          │ SMC (r0=OP-TEE function ID)
                          ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Secure World (OP-TEE OS)                       │
│                                                                   │
│  OP-TEE Core:   optee_os (built from github.com/OP-TEE/optee_os)│
│    - Thread scheduler for TA execution                           │
│    - Secure memory allocator                                     │
│    - Crypto library (libtomcrypt / mbedTLS)                     │
│    - Secure storage (using Linux filesystem via supplicant)      │
│                                                                   │
│  Trusted Applications (TAs):                                     │
│    - ta_storage.ta  ← Secure key storage                        │
│    - ta_pkcs11.ta   ← PKCS#11 hardware token                   │
│    - ta_avb.ta      ← Android Verified Boot attestation         │
│    - vendor TAs     ← DRM (Widevine L1), fingerprint            │
└──────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────────────┐
│                    ARM Trusted Firmware (TF-A)                   │
│    BL1: ROM code (boot)                                          │
│    BL2: Secure init (TZASC, PL310 secure regs, load BL31/BL32) │
│    BL31: Monitor mode handler (world switch, PSCI, SMCCC)       │
│    BL32: OP-TEE OS (secure world OS)                            │
│    BL33: Non-Secure bootloader (U-Boot)                         │
└──────────────────────────────────────────────────────────────────┘
```

### 9.2 OP-TEE Shared Memory

```c
/*
 * Shared memory between NS Linux and OP-TEE:
 * Allocated from NS Linux memory, but mapped in Secure world with NS=1
 *
 * Two types:
 * 1. Static shared memory: fixed at boot (DT: /reserved-memory/optee-shm)
 * 2. Dynamic shared memory: registered at runtime via SMC
 */

/* Linux side (optee driver): */
struct tee_shm *shm = tee_shm_alloc(ctx, size,
                                     TEE_SHM_MAPPED | TEE_SHM_DMA_BUF);
/* Internally:
 *   1. Allocate physically contiguous NS pages
 *   2. Send PA to Secure world via OPTEE_SMC_REGISTER_SHM
 *   3. OP-TEE maps PA in its page table with NS=1 descriptor
 */

/* When calling into OP-TEE: pass shm PA as parameter */
/* OP-TEE reads/writes shared memory directly */
/* On return: Linux reads results from shared memory */
```

---

## 10. Qualcomm QSEE Architecture

### 10.1 QSEE Overview

```
Qualcomm QSEE (Qualcomm Secure Execution Environment):

Runs in Secure PL1 (Secure SVC mode) on Qualcomm SoCs.
Not based on OP-TEE — fully proprietary.
Handles: DRM (PlayReady/Widevine L1), biometrics, secure payments.

QSEE Specifics vs Generic TrustZone:
  1. Qualcomm SMC IDs: Different range than SMCCC standard
     QSEOS_CLIENTENV_OPEN  = 0x32000006
     QSEOS_CLIENT_SEND_DATA = 0x32000009

  2. QSEE TAs (Trusted Apps):
     Binary format: signed ELF, encrypted with OEM key
     Loaded from /firmware partition by qseecom driver
     Each TA gets isolated address space in Secure world

  3. Qualcomm SMMU integration:
     QSEE configures SMMU stream IDs for Secure-only peripherals
     (Fingerprint sensor, crypto engine DMA has NS=0 stream ID)
     NS Linux cannot access these DMA streams

  4. SCM (Secure Channel Manager) driver:
     arch/arm/mach-qcom/scm.c → qcom_scm_call()
     Wraps SMC calls with Qualcomm-specific calling convention

  5. QFPROM (OTP fuses) access:
     QSEE reads/writes fuses for secure boot, version rollback prevention
     NS Linux can read fuses but not write (TZPC protected)
```

### 10.2 Qualcomm SCM Driver

```c
/* drivers/firmware/qcom_scm-32.c */
/*
 * Qualcomm SCM (Secure Channel Manager): ARM32 implementation
 * Uses custom SMC calling convention (pre-SMCCC)
 */

static int qcom_scm_call(struct device *dev, u32 svc_id, u32 cmd_id,
                          const struct qcom_scm_desc *desc,
                          struct qcom_scm_res *res)
{
    struct arm_smccc_res smc_res;
    u32 fn_id = QCOM_SCM_FNID(svc_id, cmd_id);

    /* Qualcomm ARMv7 SMC convention:
     *   r0 = fn_id | (num_args << 12)
     *   r1, r2, r3 = first 3 arguments
     *   If more args: use a structure in secure shared buffer
     */
    arm_smccc_smc(fn_id | QCOM_SCM_LEGACY_SMC_START,
                  desc->args[0], desc->args[1], desc->args[2],
                  0, 0, 0, 0, &smc_res);

    if (res) {
        res->result[0] = smc_res.a1;
        res->result[1] = smc_res.a2;
        res->result[2] = smc_res.a3;
    }
    return (int)smc_res.a0 ? qcom_scm_remap_error(smc_res.a0) : 0;
}

/* Example: Enable secure DMA region */
int qcom_scm_mem_protect_region(u64 start, u64 size)
{
    struct qcom_scm_desc desc = {
        .svc = QCOM_SCM_SVC_MP,
        .cmd = QCOM_SCM_MP_ASSIGN,
        .args[0] = start,
        .args[1] = size,
        .args[2] = QCOM_SCM_PERM_RW,   /* Secure RW */
    };
    return qcom_scm_call(NULL, &desc, NULL);
}
```

---

## 11. Google Trusty TEE

### 11.1 Trusty Architecture

```
Google Trusty TEE (used in Android devices, Pixel phones):

Architecture differences from OP-TEE:
  1. Based on LK (Little Kernel) microkernel
  2. Supports IPC between Trusted Apps (channels)
  3. Trusty API: tipc (Trusty IPC)
  4. Used for: Gatekeeper, Keystore, Widevine L1

┌──────────────────────────────────────────────────────────────────┐
│                    Android (Non-Secure)                           │
│  HAL: android.hardware.keymaster → hwbinder → keystore2 daemon  │
│  Trusty client: /dev/trusty-ipc-dev0                            │
│  Kernel driver: drivers/trusty/trusty.c                         │
└──────────────────────────────────────────────────────────────────┘
                          │ SMC (Trusty SMCCC function IDs)
                          ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Trusty (Secure World)                          │
│  LK microkernel (lk/trusty)                                     │
│  Trusted Apps:                                                   │
│    gatekeeper.ta    ← PIN/Pattern/Password verification         │
│    keymaster.ta     ← Hardware-backed key storage               │
│    widevine.ta      ← DRM L1 decryption                        │
│    storage_server   ← Secure persistent storage                 │
└──────────────────────────────────────────────────────────────────┘

Trusty IPC (tipc):
  Channel-based communication (like Unix domain sockets)
  tipc_connect(dev, "com.android.keymaster")
  tipc_send(channel, req_buf, req_len)
  tipc_recv(channel, rsp_buf, rsp_len)
```

---

## 12. Secure Boot and Chain of Trust

### 12.1 ARMv7-A Secure Boot Flow

```
Power-On → ROM Code (BL0)
  ↓ Validates signature of BL1
BL1 (Secure ROM / SRAM)
  ↓ Validates BL2 signature (RSA-2048 + SHA-256)
BL2 (Secure RAM — loaded by BL1)
  ↓ Configures TZASC, TZPC, clocks, DDR
  ↓ Loads and validates BL31, BL32, BL33
BL31 (Monitor mode: TF-A runtime, in SRAM or secure DRAM)
  ↓ Loads Secure OS
BL32 (OP-TEE OS or QSEE — runs in Secure PL1)
BL33 (Non-Secure bootloader: U-Boot or UEFI)
  ↓ Validates kernel image (FIT image / UEFI SecureBoot)
Linux Kernel
  ↓ Validates userspace (dm-verity, Android Verified Boot)
Android / Userspace
```

### 12.2 Anti-Rollback via OTP Fuses

```c
/*
 * Secure boot version enforcement:
 * Each software component has a minimum version encoded in OTP fuses.
 * If software version < fuse version → BOOT REFUSED
 * Prevents downgrade attacks (roll back to vulnerable version)
 *
 * Qualcomm QFPROM example:
 */
struct qfprom_sec_ver {
    u32 apss_boot_version;   /* Minimum APSS (application processor) boot version */
    u32 tz_version;          /* Minimum TrustZone version */
    u32 hlos_version;        /* Minimum Linux/Android version */
};

/* QSEE reads fuse values and compares with image header version */
/* If image_version < fuse_version: abort boot */
/* If image_version > fuse_version: optionally blow fuses to update */
```

---

## 13. Security Vulnerabilities and Mitigations

### 13.1 Common TrustZone Vulnerabilities

```
1. Shared Memory Race Condition (Time-of-Check vs Time-of-Use)
   - NS attacker modifies shared buffer AFTER TE validates it
   - Mitigation: Copy input from shared memory to private secure memory
                 before validation

2. Integer Overflow in SMC Parameter Handling
   - size + offset overflows → heap buffer overflow in Secure world
   - Mitigation: Strict bounds check with overflow-safe arithmetic

3. TZASC Misconfiguration
   - Secure memory not properly protected → NS can read keys
   - Mitigation: Verify TZASC config in BL31 before enabling NS world

4. Spectre/Meltdown through Secure→NS boundary
   - Cache side-channel: NS measures timing of Secure operations
   - Mitigation: Cache flush on world switch (performance cost ~500 cycles)
                 Constant-time crypto implementations

5. Physical Memory Attack (DRAM attacks via DMA)
   - NS DMA engine writes to Secure memory if TZASC not configured
   - Mitigation: TZASC blocks DMA transactions with NS=1 to Secure regions
                 All DMA engines must have NS=1 forced by hardware

6. Bootloader downgrade
   - Flash old vulnerable bootloader → bypass Secure Boot
   - Mitigation: Anti-rollback fuses (OTP, irreversible)
```

### 13.2 Secure World Attack Surface

```
Attack surface for Qualcomm/Google QSEE/Trusty:

1. SMC interface: Every SMC handler is an attack vector
   - 2018: CVE-2017-18175 — Qualcomm QSEE SMC buffer overflow
   - Mitigation: Fuzz SMC interface (AFL/Syzkaller port to Secure world)

2. Trusted App vulnerabilities:
   - TA has privileged access within Secure world
   - Buggy TA → compromise Secure world kernel
   - Mitigation: TA sandboxing (OP-TEE uses per-TA MMU isolation)

3. Timing side-channels:
   - RSA/ECDSA operations → timing variation reveals key bits
   - Mitigation: Constant-time crypto, blinding

4. Secure Storage attacks:
   - Brute-force attack on encrypted Secure Storage file
   - Mitigation: Rate limiting in Secure world, hardware-backed counter
```

---

## Summary

| Component | Purpose | Implementation |
|-----------|---------|----------------|
| SCR | NS bit, FIQ/IRQ routing | Monitor mode CP15 |
| TZASC (TZC-400) | DRAM access control | AXI bus filter |
| TZPC | Peripheral access control | APB bus controller |
| Monitor Mode | World switch handler | TF-A BL31 |
| OP-TEE | Open source TEE OS | github.com/OP-TEE |
| QSEE | Qualcomm proprietary TEE | Closed source |
| Trusty | Google TEE on LK kernel | AOSP |
| SMCCC | Standard SMC calling convention | DEN0028 spec |
| PSCI | Power management via SMC | TF-A BL31 |

---

**Cross-References:**
- Doc 01: NS bit in page descriptors
- Doc 02: Secure world in boot sequence (BL1/BL2/BL31/BL32)
- Doc 07: SMMU secure stream isolation (QSEE + SMMU)
- Doc 09: Stage-2 page tables for hypervisor (complements Secure world isolation)

---
**End of Document 6**
