# SCM — Secure Channel Manager

## Overview

**SCM (Secure Channel Manager)** is the kernel-side interface for making **SMC (Secure Monitor Call)** requests to TrustZone. Every time the Normal World (Linux kernel) needs a secure operation — PIL authentication, fuse reading, secure I/O, or content protection — it goes through SCM.

---

## SCM Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Linux Kernel (EL1, Normal World)                            │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  SCM Driver (drivers/firmware/qcom_scm.c)               │ │
│  │                                                         │ │
│  │  API Functions:                                         │ │
│  │  ├── qcom_scm_pas_init_image()  → PIL auth init       │ │
│  │  ├── qcom_scm_pas_auth_and_reset() → PIL auth+boot    │ │
│  │  ├── qcom_scm_set_cold_boot_addr() → CPU boot addr    │ │
│  │  ├── qcom_scm_io_readl()        → Secure register read│ │
│  │  ├── qcom_scm_io_writel()       → Secure register write│ │
│  │  └── qcom_scm_restore_sec_cfg() → Restore security cfg│ │
│  │                                                         │ │
│  │  Implementation:                                        │ │
│  │  ├── Build SMC descriptor (function ID + args)         │ │
│  │  ├── Execute SMC #0 instruction (trap to EL3)          │ │
│  │  └── Read return value from registers                  │ │
│  └─────────────────────────────────────────────────────────┘ │
│                            │ SMC                             │
│  ┌─────────────────────────▼───────────────────────────────┐ │
│  │  TrustZone (EL3, Secure Monitor)                        │ │
│  │                                                         │ │
│  │  Receives SMC, dispatches to service handler:           │ │
│  │  ├── SVC_BOOT:  Boot-related operations                │ │
│  │  ├── SVC_PIL:   Peripheral Image Loader auth           │ │
│  │  ├── SVC_IO:    Secure I/O (protected registers)       │ │
│  │  ├── SVC_POWER: Power management coordination          │ │
│  │  └── SVC_CP:    Content protection (DRM)               │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

---

## SCM Call Flow

```
Kernel driver needs secure operation (e.g., PIL auth):
───────────────────────────────────────────────────────
1. qcom_scm_pas_auth_and_reset(MODEM_PAS_ID)

2. SCM driver builds descriptor:
   ├── SVC ID: 0x04 (SVC_PIL)
   ├── CMD ID: 0x05 (PAS_AUTH_AND_RESET)
   ├── args[0]: MODEM_PAS_ID (0x04)
   └── Owner: SCM_ARMV8 (64-bit calling convention)

3. arm_smccc_smc(function_id, arg0, arg1, ..., &res)
   ├── Registers: x0=func_id, x1=arg0, x2=arg1, ...
   └── SMC #0 instruction → CPU traps to EL3

4. TrustZone:
   ├── Reads function ID from x0
   ├── Dispatches to PIL service handler
   ├── Verifies firmware signature
   ├── If PASS: releases modem from reset
   └── Returns result in x0 (0 = success)

5. SCM driver reads res.a0 → return to caller
```

---

## SCM Service Table

| Service | ID | Key Commands |
|---------|-----|-------------|
| SVC_BOOT | 0x01 | Set boot address, cold/warm boot |
| SVC_PIL | 0x04 | PAS init, mem setup, auth, shutdown |
| SVC_IO | 0x05 | Secure register read/write |
| SVC_INFO | 0x06 | Query TZ version, features |
| SVC_POWER | 0x09 | Power collapse coordination |
| SVC_CP | 0x0C | Content protection (Widevine) |
| SVC_DCVS | 0x0D | Dynamic clock/voltage scaling |
| SVC_OCMEM | 0x0F | On-chip memory management |

---

## Usage Examples

```c
/* PIL: Authenticate and boot modem firmware */
ret = qcom_scm_pas_auth_and_reset(PAS_MODEM);
if (ret)
    pr_err("Modem authentication failed: %d\n", ret);

/* Secure I/O: Read a protected register */
u32 val;
ret = qcom_scm_io_readl(TCSR_BOOT_MISC_DETECT, &val);

/* PSCI: Set warm boot address for CPU */
ret = qcom_scm_set_warm_boot_addr(entry_point, cpu);
```

---

## Debugging SCM

```bash
# TZ version
adb shell dmesg | grep "scm\|tz"
# [    0.100] qcom_scm firmware:qcom_scm: SCM version: v1.1

# SCM call failure
# Kernel log: "qcom_scm_call failed: ret = -22"
# -22 = -EINVAL → invalid arguments
# -13 = -EACCES → permission denied (wrong signature)
```

---

## Related Documents

- [../02_XBL_Secondary_Bootloader/05_TrustZone_QSEE_Init.md](../02_XBL_Secondary_Bootloader/05_TrustZone_QSEE_Init.md) — TZ initialization
- [../07_Subsystem_Loading/01_PIL_Framework.md](../07_Subsystem_Loading/01_PIL_Framework.md) — PIL uses SCM for auth
- [../05_Linux_Kernel_Boot/07_PSCI_CPU_Bringup.md](../05_Linux_Kernel_Boot/07_PSCI_CPU_Bringup.md) — PSCI also uses SMC calls
