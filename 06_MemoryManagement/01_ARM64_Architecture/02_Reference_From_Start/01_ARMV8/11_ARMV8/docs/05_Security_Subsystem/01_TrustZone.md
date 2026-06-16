# TrustZone Architecture

## 1. What Is TrustZone?

TrustZone is a **hardware security extension** built into every ARMv8-A core. It creates two isolated execution environments — **Secure World** and **Normal World** — enforced by the CPU and bus fabric hardware.

```
Key idea: The CPU has a single bit (NS bit in SCR_EL3) that determines
which world is currently executing. All bus transactions carry this bit,
so memory controllers and peripherals can enforce access policies.

┌──────────────────────────────────────────────────────────────┐
│                    Processor Core                             │
│                                                               │
│   ┌─────────────────┐         ┌─────────────────┐           │
│   │  Normal World   │  SMC    │  Secure World   │           │
│   │                  │◄──────►│                  │           │
│   │ EL0: User apps   │         │ S-EL0: Secure    │           │
│   │ EL1: OS kernel   │         │        apps      │           │
│   │ EL2: Hypervisor  │         │ S-EL1: Secure OS │           │
│   │                  │         │                  │           │
│   └─────────────────┘         └─────────────────┘           │
│              │                         │                      │
│              └────────┬────────────────┘                      │
│                       │                                       │
│              ┌────────▼────────┐                              │
│              │  EL3: Secure    │                              │
│              │  Monitor (ATF)  │                              │
│              └─────────────────┘                              │
│                                                               │
│   NS bit = 0 → Secure World                                  │
│   NS bit = 1 → Normal World                                  │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. SCR_EL3 — Secure Configuration Register

The Secure Monitor at EL3 controls world switching via SCR_EL3:

```
SCR_EL3 Fields (key bits):
┌─────┬────────────────────────────────────────────────────────┐
│ Bit │ Name  │ Function                                       │
├─────┼───────┼────────────────────────────────────────────────┤
│  0  │ NS    │ Non-Secure: 0=Secure, 1=Normal world          │
│  1  │ IRQ   │ 1=Route IRQ to EL3                            │
│  2  │ FIQ   │ 1=Route FIQ to EL3                            │
│  3  │ EA    │ 1=Route SError to EL3                         │
│  7  │ SMD   │ 1=Disable SMC instruction                     │
│  8  │ HCE   │ 1=Enable HVC instruction                     │
│ 10  │ RW    │ 1=EL2/EL1 is AArch64, 0=AArch32              │
│ 11  │ ST    │ 1=Secure EL1 can access Secure timer          │
│ 12  │ TWI   │ 1=Trap WFI to EL3                            │
│ 13  │ TWE   │ 1=Trap WFE to EL3                            │
│ 21  │ FIEN  │ 1=Enable FEAT_ExS                            │
│ 25  │ EnSCXT│ 1=Enable SCXTNUM_ELx registers               │
│ 26  │ HXEn  │ 1=Enable HCRX_EL2                            │
└─────┴───────┴────────────────────────────────────────────────┘

World switch sequence (Normal → Secure):
  1. Normal world executes SMC instruction
  2. CPU takes exception to EL3 (Secure Monitor)
  3. Monitor saves Normal world context (all registers)
  4. Monitor sets SCR_EL3.NS = 0
  5. Monitor restores Secure world context
  6. ERET to Secure EL1
```

---

## 3. SMC — Secure Monitor Call

```
SMC is the gateway between worlds:

  Normal World:                   Secure World:
  ┌──────────────────┐           ┌──────────────────┐
  │ Linux Kernel     │           │ OP-TEE (Secure OS)│
  │                  │           │                    │
  │ Want to use      │           │ Trusted App runs   │
  │ secure service:  │           │ in isolated env    │
  │                  │           │                    │
  │ MOV X0, #func_id│           │ Result in X0-X3    │
  │ MOV X1, #param1 │           │                    │
  │ MOV X2, #param2 │           │                    │
  │ SMC #0           │──────────►│ Handle SMC        │
  │                  │           │ Execute trusted    │
  │ (blocked until   │◄──────────│ operation          │
  │  SMC returns)    │           │ SMC return         │
  │                  │           │                    │
  │ Read result X0   │           │                    │
  └──────────────────┘           └──────────────────────┘

SMC Calling Convention (SMCCC):
  X0: Function ID
    ┌───────────────────────────────────────────────────┐
    │ [31]    Fast/Yielding: 1=Fast, 0=Yielding         │
    │ [30]    SMC64/SMC32: 1=64-bit, 0=32-bit           │
    │ [29:24] Service range:                            │
    │           0x00-0x01 = ARM Architecture Calls       │
    │           0x02-0x0F = CPU Service Calls           │
    │           0x0A      = Trusted OS Calls            │
    │           0x30-0x31 = Trusted App Calls           │
    │ [15:0]  Function number                           │
    └───────────────────────────────────────────────────┘
  X1-X7: Parameters
  Return: X0-X3

Common SMC calls:
  PSCI_CPU_ON     = 0xC400_0003  (power on a core)
  PSCI_CPU_OFF    = 0x8400_0002  (power off calling core)
  PSCI_SYSTEM_OFF = 0x8400_0008  (system shutdown)
  PSCI_SYSTEM_RST = 0x8400_0009  (system reset)
```

---

## 4. TrustZone Address Space Controller (TZASC)

```
TZASC partitions DRAM into Secure and Non-Secure regions:

  Physical Memory Map:
  ┌────────────────────┐ 0xFFFF_FFFF
  │                    │
  │  Normal World DRAM │ ← Non-secure access only
  │                    │
  ├────────────────────┤ 0x8800_0000
  │  Secure DRAM       │ ← Secure access only
  │  (Secure OS, keys) │   Normal world reads → BUS ERROR
  ├────────────────────┤ 0x8000_0000
  │  Shared Buffer     │ ← Both worlds can access
  ├────────────────────┤ 0x7F00_0000
  │  Normal World DRAM │
  │  (Linux kernel)    │
  ├────────────────────┤ 0x4000_0000
  │  ...               │
  └────────────────────┘ 0x0000_0000

TZASC registers configure regions:
  Region 0: default (e.g., Non-Secure)
  Region 1-8: configurable start/end + security
  
  TZASC_REGION_SETUP_LOW_n:  start address
  TZASC_REGION_SETUP_HIGH_n: end address
  TZASC_REGION_ATTRIBUTES_n:
    SP[3:0] = security permissions
      0x0 = No access
      0x1 = Secure read/write only
      0x3 = Non-secure read/write allowed
      0xF = Full access both worlds
```

---

## 5. TrustZone Protection Controller (TZPC)

```
TZPC controls whether peripherals are Secure or Non-Secure:

  ┌──────────────────────────────────────────────────────┐
  │  SoC Peripherals                                      │
  │                                                        │
  │  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐             │
  │  │UART0 │  │ SPI  │  │Crypto│  │ RNG  │             │
  │  │ NS   │  │ NS   │  │  S   │  │  S   │             │
  │  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘             │
  │     │         │         │         │                    │
  │  ┌──▼─────────▼─────────▼─────────▼──┐               │
  │  │           TZPC                      │               │
  │  │  Bit 0 = UART0   → NS=1 (Normal)  │               │
  │  │  Bit 1 = SPI     → NS=1 (Normal)  │               │
  │  │  Bit 2 = Crypto  → NS=0 (Secure)  │               │
  │  │  Bit 3 = RNG     → NS=0 (Secure)  │               │
  │  └────────────────────────────────────┘               │
  │                                                        │
  │  If Normal World accesses Crypto → bus fault          │
  │  If Normal World accesses UART0  → allowed            │
  └──────────────────────────────────────────────────────┘

  Peripherals commonly marked Secure:
  • Crypto accelerator (AES/SHA hardware)
  • True Random Number Generator (TRNG)
  • One-Time-Programmable (OTP/eFuse) memory
  • Secure watchdog timer
  • Key storage (CAAM, CE, etc.)
```

---

## 6. Realm Management Extension (RME) — ARMv9 CCA

```
ARMv9 adds TWO MORE security states:

  ARMv8 TrustZone:  2 worlds (Secure, Normal)
  ARMv9 CCA/RME:    4 worlds (Secure, Normal, Realm, Root)

  ┌────────────────────────────────────────────────────────┐
  │                                                         │
  │  ┌─────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
  │  │ Root    │ │ Secure   │ │ Normal   │ │ Realm    │  │
  │  │ (EL3)   │ │ (S-EL1)  │ │ (EL1/2)  │ │ (R-EL1/2)│  │
  │  │ Monitor │ │ Secure OS│ │ Rich OS  │ │ Realm VM │  │
  │  │ (ATF)   │ │ (OP-TEE) │ │ (Linux)  │ │ (CCA VM) │  │
  │  └─────────┘ └──────────┘ └──────────┘ └──────────┘  │
  │                                                         │
  │  Realm = protected VM that even the hypervisor          │
  │          cannot read/modify (for confidential computing)│
  │                                                         │
  │  Granule Protection Table (GPT): per-page ownership     │
  │    Each physical page tagged: Root/Secure/Normal/Realm  │
  │    Hardware enforces: Normal code can't access Realm    │
  │    memory, even if hypervisor maps it                   │
  └────────────────────────────────────────────────────────┘
```

---

## 7. Pointer Authentication (PAC) — ARMv8.3

```
PAC adds cryptographic signatures to pointers to prevent ROP/JOP attacks:

  Regular pointer (64-bit):
  ┌─────────────────────────────────────────────────────────┐
  │ [63:48] Unused/sign │ [47:0] Virtual Address            │
  └─────────────────────────────────────────────────────────┘

  PAC-signed pointer:
  ┌─────────────────────────────────────────────────────────┐
  │ [63:56] PAC │ [55:48] PAC │ [47:0] Virtual Address      │
  └─────────────────────────────────────────────────────────┘
  
  PAC = QARMA(Key, Address, Context)  ← hardware crypto MAC

  Instructions:
    PACIA X30, SP     // Sign return address with key A + SP context
    RETAA             // Verify PAC, strip it, then RET
                      // If PAC invalid → fault (prevents ROP)

    PACIB X16, X17    // Sign pointer with key B
    AUTIB X16, X17    // Authenticate pointer (fault if tampered)

  Keys (per-EL):
    APIAKeyHi/Lo_EL1  — Instruction key A
    APIBKeyHi/Lo_EL1  — Instruction key B
    APDAKeyHi/Lo_EL1  — Data key A
    APDBKeyHi/Lo_EL1  — Data key B
    APGAKeyHi/Lo_EL1  — Generic key

  Linux: enabled via CONFIG_ARM64_PTR_AUTH, GCC -msign-return-address
```

---

## 8. Memory Tagging Extension (MTE) — ARMv8.5

```
MTE detects memory safety bugs (use-after-free, buffer overflow):

  Every 16-byte memory granule gets a 4-bit "tag" (stored in RAM):
  
  Physical Memory:
  ┌────────┬──────────────────────┐
  │ Tag=3  │ 16 bytes of data     │  addr 0x1000
  ├────────┼──────────────────────┤
  │ Tag=7  │ 16 bytes of data     │  addr 0x1010
  ├────────┼──────────────────────┤
  │ Tag=3  │ 16 bytes of data     │  addr 0x1020
  └────────┴──────────────────────┘

  Pointers carry a tag in bits [59:56]:
  ┌─────────────────────────────────────────────────────────┐
  │ [63:60] │ [59:56] Tag=3 │ [55:0] Address               │
  └─────────────────────────────────────────────────────────┘

  On memory access: if pointer_tag ≠ memory_tag → FAULT

  Instructions:
    IRG  X0, X0        // Insert Random tag into pointer
    STG  X0, [X0]      // Store tag to memory (set allocation tag)
    LDG  X0, [X1]      // Load tag from memory into pointer
    ADDG X0, X1, #16, #3 // Add offset + set tag

  Catches:
  ┌──────────────────────────────────────────────┐
  │ Bug Type           │ How MTE catches it       │
  ├────────────────────┼──────────────────────────┤
  │ Buffer overflow    │ Adjacent buffer has       │
  │                    │ different tag → mismatch  │
  │ Use-after-free     │ Free changes tag →        │
  │                    │ stale pointer mismatch    │
  │ Double free        │ Tag already changed       │
  └────────────────────┴──────────────────────────┘

  Linux: CONFIG_ARM64_MTE, Android uses MTE in production
```

---

Next: [Secure Boot Chain →](./02_Secure_Boot.md) | Back to [Security Subsystem Overview](./README.md)
