# TrustZone & QSEE Initialization

## Overview

**TrustZone** is ARM's hardware security technology that divides the SoC into two worlds: **Secure World** and **Non-Secure (Normal) World**. Qualcomm's implementation is called **QSEE (Qualcomm Secure Execution Environment)**. XBL loads the TrustZone firmware from the `tz` partition and initializes the secure world before any non-secure code (ABL, kernel) can run.

---

## ARM Exception Levels & TrustZone

```
┌──────────────────────────────────────────────────────┐
│                ARM Exception Levels                    │
│                                                      │
│  ┌──────────────────────────────────────────────┐    │
│  │  EL3: Secure Monitor (highest privilege)      │    │
│  │  ├── Runs: TrustZone firmware                │    │
│  │  ├── Controls world switching (NS bit)       │    │
│  │  └── SMC handler (Secure Monitor Call)       │    │
│  └──────────────────────────────────────────────┘    │
│                                                      │
│  ┌──────────────────┐  ┌──────────────────┐         │
│  │  Secure World    │  │  Normal World    │         │
│  │                  │  │                  │         │
│  │  EL1: QSEE      │  │  EL2: Hypervisor │         │
│  │  (Secure OS)     │  │  (optional)      │         │
│  │                  │  │                  │         │
│  │  EL0: Trusted    │  │  EL1: Linux      │         │
│  │  Applications    │  │  Kernel          │         │
│  │  (TAs)           │  │                  │         │
│  │  - Keymaster     │  │  EL0: Android    │         │
│  │  - DRM (Widevine)│  │  Apps            │         │
│  │  - Fingerprint   │  │                  │         │
│  └──────────────────┘  └──────────────────┘         │
│                                                      │
│  NS=0 (Secure)          NS=1 (Non-Secure)           │
└──────────────────────────────────────────────────────┘
```

---

## TrustZone Initialization by XBL

```
XBL (after DDR and PMIC init)
    │
    ▼
┌──────────────────────────────────────────────────────┐
│            TrustZone Initialization                   │
│                                                      │
│  Step 1: Load TZ firmware                            │
│  ├── Read tz partition from flash                    │
│  ├── Load TZ image to secure DDR region              │
│  └── Authenticate TZ (RSA signature verify)          │
│                                                      │
│  Step 2: Configure secure memory regions             │
│  ├── Mark DDR regions as Secure (SMMU/xPU)          │
│  │   └── TZ code/data: 0x8620_0000 - 0x8880_0000   │
│  ├── Mark IMEM regions                               │
│  └── Configure TrustZone Protection Unit (xPU)       │
│      └── Prevents Normal World from accessing        │
│          secure memory                               │
│                                                      │
│  Step 3: Initialize EL3 exception vectors            │
│  ├── Set VBAR_EL3 (Exception Vector Base Address)    │
│  ├── Install SMC handler for world switching         │
│  └── Configure SCR_EL3 (Secure Config Register)      │
│      ├── NS bit: controls which world is active      │
│      ├── IRQ/FIQ routing to appropriate world        │
│      └── HCE: Hypervisor call enable                 │
│                                                      │
│  Step 4: Load supporting TZ components               │
│  ├── Load keymaster TA from keymaster partition      │
│  ├── Load cmnlib from cmnlib partition               │
│  ├── Load cmnlib64 from cmnlib64 partition           │
│  └── Load devcfg from devcfg partition               │
│                                                      │
│  Step 5: Initialize crypto hardware                  │
│  ├── PRNG (Pseudo-Random Number Generator)           │
│  ├── Crypto engine (AES, SHA, RSA HW accelerator)   │
│  └── Fuse controller (for reading OTP fuses)         │
│                                                      │
│  Step 6: TZ ready                                    │
│  └── TZ firmware running at EL3, waiting for SMC     │
│      calls from Normal World                         │
└──────────────────────────────────────────────────────┘
    │
    ▼
XBL continues: loads RPM FW, then ABL
```

---

## Secure Monitor Call (SMC) Interface

The Normal World (Linux kernel) communicates with TrustZone via **SMC (Secure Monitor Call)** instructions:

```
Linux Kernel (EL1, Normal World)
    │
    ├── SMC #0 instruction
    │   (CPU traps to EL3)
    │
    ▼
TrustZone (EL3, Secure World)
    │
    ├── Reads SMC function ID from registers
    ├── Executes secure operation
    ├── Returns result
    │
    ▼
Linux Kernel continues (EL1)
```

### Common SCM (Secure Channel Manager) Calls

| SCM Function | Purpose | Called By |
|-------------|---------|-----------|
| `SCM_SVC_BOOT` | Boot services (PIL auth) | PIL driver |
| `SCM_SVC_PIL` | Peripheral Image Loader auth | PIL driver |
| `SCM_SVC_IO` | Secure I/O access | Various drivers |
| `SCM_SVC_INFO` | System info queries | Kernel init |
| `SCM_SVC_POWER` | Power collapse coordination | PM driver |
| `SCM_SVC_CP` | Content protection (DRM) | DRM framework |
| `SCM_SVC_DCVS` | Secure DCVS | CPUFreq |

### Kernel SCM Driver

```c
/* drivers/firmware/qcom_scm.c */

/* Example: PIL authentication request */
int qcom_scm_pas_auth_and_reset(u32 peripheral_id)
{
    struct qcom_scm_desc desc = {
        .svc = SCM_SVC_PIL,
        .cmd = PAS_AUTH_AND_RESET,
        .args[0] = peripheral_id,
    };
    return qcom_scm_call(&desc);
    /* This triggers SMC → TrustZone verifies firmware signature */
}
```

---

## Memory Protection (xPU)

TrustZone uses **xPU (Access Control Units)** to enforce memory protection:

```
DDR Physical Memory Map:
─────────────────────────
┌───────────────────────┐
│  Normal World Memory   │ ← Linux kernel, apps
│  (NS = 1)              │    Can be accessed by Normal World
├───────────────────────┤
│  Secure Memory         │ ← TZ code, TAs, crypto keys
│  (NS = 0)              │    ONLY accessible by Secure World
│                        │    Any Normal World access → bus error
├───────────────────────┤
│  Shared Memory (SMEM)  │ ← Accessible by both worlds
│  (configured per item) │    Some items secure-only
├───────────────────────┤
│  PIL Memory            │ ← Firmware loading region
│  (XPU protected)       │    TZ authenticates, then locks
└───────────────────────┘
```

---

## Trusted Applications (TAs)

QSEE hosts **Trusted Applications** that run in the Secure World:

| TA | Partition | Purpose |
|----|-----------|---------|
| Keymaster | `keymaster` | Hardware-backed key storage (Android Keystore) |
| Gatekeeper | embedded in TZ | Credential verification (PIN/pattern) |
| Widevine DRM | loaded by DRM HAL | Content protection for streaming |
| Fingerprint | loaded by FP HAL | Fingerprint template storage |
| FIDO | loaded by FIDO HAL | FIDO authentication |

---

## TrustZone Boot Verification

```
Fuse-based Root of Trust:
─────────────────────────
OEM Root Certificate Hash (SHA-256) → burned in OTP fuses
    │
    ▼
PBL verifies XBL → XBL verifies TZ → TZ verifies ABL
    │
    ▼
TZ also verifies:
├── All PIL firmware (modem, ADSP, CDSP, SLPI)
├── Trusted Applications loaded at runtime
└── boot.img (via Android Verified Boot / dm-verity)
```

---

## Debugging TrustZone Issues

| Issue | Symptom | Debug |
|-------|---------|-------|
| TZ authentication fails | XBL halts, no boot | Re-sign TZ with correct OEM key |
| SCM call fails | Kernel panic: "scm call failed" | Check TZ version compatibility |
| Secure memory violation | Bus error / data abort | Normal world code accessing secure region |
| Keymaster init fails | Android "secure boot not available" | Check keymaster partition, TZ logs |

### Viewing TZ Version

```bash
# TZ version in kernel log
adb shell dmesg | grep -i "qcom,tz"

# TZ build info
adb shell cat /sys/firmware/devicetree/base/qcom,msm-id
```

---

## Related Documents

- [01_XBL_Overview.md](01_XBL_Overview.md) — XBL loads TZ
- [../08_IPC_Mechanisms/06_SCM_Secure_Channel.md](../08_IPC_Mechanisms/06_SCM_Secure_Channel.md) — SCM interface deep-dive
- [../07_Subsystem_Loading/01_PIL_Framework.md](../07_Subsystem_Loading/01_PIL_Framework.md) — TZ authenticates PIL firmware
