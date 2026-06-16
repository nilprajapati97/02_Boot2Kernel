# PBL — Primary Boot Loader (ROM)

## Overview

The **PBL (Primary Boot Loader)** is the very first code that executes on the SDM660 after power-on reset. It is **burned into on-chip ROM** during manufacturing and **cannot be updated**. Its job is minimal but critical: validate and load the next boot stage (XBL) from flash storage.

---

## PBL Characteristics

| Property | Value |
|----------|-------|
| Location | On-chip ROM (mask ROM) |
| Updatable | No — burned at factory |
| Size | ~32 KB |
| Executes on | CPU0 (Gold Core 0) at reduced clock |
| Memory available | IMEM/OCIMEM (~256 KB on-chip SRAM) |
| DDR available | No — DDR not yet initialized |
| Security | Qualcomm secure boot root of trust |

---

## PBL Execution Flow

```
Power-On Reset (POR) deasserts
    │
    ▼
CPU0 fetches first instruction from ROM
(Reset vector: 0x0000_0000 or SoC-specific ROM address)
    │
    ▼
┌─────────────────────────────────────────────┐
│              PBL Execution                   │
│                                             │
│  1. Minimal hardware init                    │
│     ├── Set up stack in IMEM                │
│     ├── Initialize basic clocks (XO → PLL)  │
│     └── Initialize SPMI (for PMIC access)   │
│                                             │
│  2. Determine boot device                    │
│     ├── Read boot config fuses/GPIO          │
│     ├── eMMC? → Initialize SDHCI controller  │
│     └── UFS?  → Initialize UFS controller    │
│                                             │
│  3. Load XBL from flash                      │
│     ├── Read xbl partition from storage      │
│     ├── Load XBL image to IMEM              │
│     └── Image format: ELF or MBN            │
│                                             │
│  4. Authenticate XBL                         │
│     ├── Read OEM public key hash from fuses  │
│     ├── Verify XBL RSA signature            │
│     ├── Verify XBL SHA-256 hash             │
│     └── If auth fails → PBL halts (no boot) │
│                                             │
│  5. Jump to XBL entry point                  │
│     └── CPU0 begins executing XBL code      │
└─────────────────────────────────────────────┘
```

---

## CPU State During PBL

When PBL starts, the CPU is in a very primitive state:

```
CPU State at PBL entry:
───────────────────────
  Exception Level: EL3 (highest privilege — secure monitor)
  MMU: OFF (no virtual memory)
  Caches: OFF (or minimal)
  Clock: Running on XO (19.2 MHz crystal) or low-freq PLL
  Memory: IMEM only (~256 KB SRAM, no DDR)
  Interrupts: Disabled
  Other CPUs: Powered off (only CPU0 runs)
  Security: Secure world (TrustZone not yet initialized)
```

### Why EL3?

ARM defines 4 exception levels:

```
EL3: Secure Monitor (PBL, TrustZone firmware)
EL2: Hypervisor (optional)
EL1: OS Kernel (Linux)
EL0: User applications (Android apps)

PBL runs at EL3 because it's the root of trust — 
it must be able to configure security before anything else.
```

---

## Boot Device Detection

PBL determines which storage device to boot from by reading hardware straps or fuses:

```
Boot Config Fuses (OTP — One-Time Programmable)
    │
    ├── Value 0x0: UFS boot
    ├── Value 0x1: eMMC boot
    ├── Value 0x2: SD card boot (debug only)
    └── Value 0x3: USB boot (emergency download mode, EDL)

If no valid boot device found:
    └── PBL enters EDL (Emergency Download) mode
        └── Device appears as Qualcomm HS-USB QDLoader 9008
        └── Used for factory flashing via QFIL tool
```

### Emergency Download Mode (EDL)

If PBL cannot find or authenticate XBL, it enters **EDL (9008) mode**:

```
PC ◀──── USB ────▶ SDM660 in EDL mode
│                       │
│  QFIL / QPST tool     │  Sahara protocol
│  sends firehose        │  PBL loads firehose
│  programmer            │  programmer to IMEM
│                       │
│  Firehose writes      │  Flash storage
│  partition images     │  partitions restored
│                       │
└───────────────────────┘

EDL is the "last resort" recovery mechanism.
```

---

## Secure Boot Chain (Root of Trust)

PBL is the **root of trust** — the entire secure boot chain starts here:

```
┌────────────────────────────────────────────────────┐
│                  SECURE BOOT CHAIN                  │
│                                                    │
│  ROM Fuses (OTP)                                   │
│  ┌─────────────────────────────┐                   │
│  │ OEM Public Key Hash         │                   │
│  │ (SHA-256 of OEM root cert) │                   │
│  │ Burned once, never changes  │                   │
│  └─────────────┬───────────────┘                   │
│                │                                   │
│  PBL (ROM)     │ compares hash                     │
│  ┌─────────────▼───────────────┐                   │
│  │ Loads XBL from flash        │                   │
│  │ Reads XBL signature         │                   │
│  │ Verifies RSA signature      │                   │
│  │ using fused OEM public key  │                   │
│  └─────────────┬───────────────┘                   │
│                │ if valid                          │
│  XBL           ▼                                   │
│  ┌─────────────────────────────┐                   │
│  │ Loads TZ from flash         │                   │
│  │ Verifies TZ signature       │                   │
│  │ using OEM cert chain        │                   │
│  └─────────────┬───────────────┘                   │
│                │ if valid                          │
│  TZ            ▼                                   │
│  ┌─────────────────────────────┐                   │
│  │ Loads ABL from flash        │                   │
│  │ Verifies ABL signature      │                   │
│  └─────────────┬───────────────┘                   │
│                │ if valid                          │
│  ABL           ▼                                   │
│  ┌─────────────────────────────┐                   │
│  │ Loads kernel from flash     │                   │
│  │ Verifies boot.img           │                   │
│  │ (dm-verity / AVB)           │                   │
│  └─────────────────────────────┘                   │
│                                                    │
│  Each stage verifies the next.                      │
│  If ANY verification fails → boot stops.           │
└────────────────────────────────────────────────────┘
```

---

## PBL Failure Modes

| Failure | Behavior | Debug |
|---------|----------|-------|
| No boot device detected | Enter EDL (9008 mode) | Check boot config fuses/straps |
| XBL not found on flash | Enter EDL | Flash XBL via QFIL |
| XBL signature invalid | PBL halts or enters EDL | Re-sign XBL with correct OEM key |
| IMEM corruption | Undefined (may hang) | Hardware issue — check power rails |
| PBL ROM defect | Board is bricked | Manufacturing defect (very rare) |

---

## PBL Timeline

```
Event                          Approximate Time
──────────────────────────────────────────────
POR deasserts                  T = 0 ms
PBL starts executing           T ≈ 0.1 ms
SPMI/PMIC init                T ≈ 0.5 ms
Storage controller init         T ≈ 1 ms
XBL read from flash            T ≈ 2-5 ms
XBL authentication             T ≈ 3-8 ms
Jump to XBL                    T ≈ 5-10 ms
──────────────────────────────────────────────
Total PBL time: ~5-10 ms
```

---

## Key Takeaways for Bring-Up Engineers

1. **PBL is not modifiable** — if PBL fails, you need EDL mode or a new board
2. **XBL must be properly signed** — unsigned or wrongly-signed XBL won't boot
3. **Boot device must be configured correctly** — wrong fuse settings = no boot
4. **EDL mode (9008) is your lifeline** — always verify EDL works before modifying boot chain
5. **PS_HOLD must be set quickly** — PBL sets it; if it doesn't, PMIC shuts down

---

## Related Documents

- [01_Power_Sequence.md](01_Power_Sequence.md) — Power-up sequence before PBL
- [../02_XBL_Secondary_Bootloader/01_XBL_Overview.md](../02_XBL_Secondary_Bootloader/01_XBL_Overview.md) — What XBL does after PBL
- [../00_SDM660_Architecture/06_Partition_Layout.md](../00_SDM660_Architecture/06_Partition_Layout.md) — Where XBL lives on flash
