# Power Sequence — PMIC Power-Up & SoC Reset

## Overview

When the power button is pressed on an SDM660 device, a precise **voltage rail sequencing** orchestrated by the PM660 PMIC must occur before the SoC can begin executing code. Getting this sequence wrong during bring-up will cause the board to not boot or latch-up.

---

## Power-On Trigger

```
┌──────────────┐
│   Battery    │
│   (3.7-4.2V) │
└──────┬───────┘
       │
       ▼
┌──────────────┐     ┌──────────────┐
│  PM660 PMIC  │────▶│  PM660L PMIC │
│              │     │              │
│  KPDPWR pin  │     │  (Slave)     │
│  ◀── Power   │     │              │
│      Button  │     │              │
└──────┬───────┘     └──────────────┘
       │
       │ SPMI + Power Rails
       ▼
┌──────────────┐
│  SDM660 SoC  │
│              │
│  PS_HOLD pin │──▶ (SoC holds PMIC on after boot)
│  POR pin     │◀── (Power-On Reset from PMIC)
└──────────────┘
```

### Power-On Sources

| Source | Trigger | Use Case |
|--------|---------|----------|
| KPDPWR (Power Key) | User presses power button | Normal power-on |
| USB/DCIN | Charger connected | Power-on while charging |
| RTC Alarm | Scheduled alarm | Scheduled wake-up |
| PON1 (External) | External signal | Debug/test |

---

## Voltage Rail Sequencing

The PMIC powers rails in a strict order with specific timing delays. Each rail must stabilize before the next begins.

```
Time ──────────────────────────────────────────────────────────▶

KPDPWR pressed
│
├── T0: PON module detects key press
│   └── PM660 internal state machine starts
│
├── T1 (+0 ms): VDD_DDR (S5: 1.35V)
│   └── DDR power must be first (memory needs stable power)
│
├── T2 (+0.5 ms): VDD_CX (S1: 0.87V)
│   └── Core logic power (NoC, config registers)
│   └── Required before any digital logic operates
│
├── T3 (+1 ms): VDD_MX (S2: 1.05V)
│   └── Memory retention voltage (SRAM, caches)
│   └── Must be ≥ VDD_CX at all times
│
├── T4 (+1.5 ms): VDD_APC_Gold (S3: ~1.0V initial)
│   └── CPU Gold cluster power
│   └── CPU0 will execute PBL from this point
│
├── T5 (+2 ms): VDD_APC_Silver (S6: ~0.9V initial)
│   └── CPU Silver cluster power (can be delayed)
│
├── T6 (+2.5 ms): I/O LDOs (L-series)
│   └── L9 (1.8V I/O), L13 (sensor VDD), etc.
│   └── Peripheral I/O becomes available
│
├── T7 (+3 ms): POR deasserted
│   └── SDM660 comes out of reset
│   └── CPU0 fetches first instruction from ROM (PBL)
│
├── T8 (+5 ms): PS_HOLD asserted by SoC
│   └── SoC signals PMIC: "I'm alive, keep power on"
│   └── If PS_HOLD not asserted within timeout, PMIC shuts down
│
└── Boot continues → PBL → XBL → ABL → Kernel
```

---

## Critical Voltage Relationships

Several voltage rails have strict ordering requirements:

```
VDD_MX ≥ VDD_CX       (always — MX must be equal or higher than CX)
VDD_CX before VDD_APC  (core logic before CPU)
VDD_DDR before VDD_MX  (DDR before memory retention)

Violation → SoC latch-up, undefined behavior, or no boot
```

### Voltage Domain Hierarchy

```
                    VDD_DDR (1.35V)
                        │
                        ▼
                    VDD_MX (1.05V)
                        │
              ┌─────────┼─────────┐
              ▼         ▼         ▼
          VDD_CX    VDD_LPI_MX  VDD_GFX
          (0.87V)   (0.7V)      (0.7V)
              │         │
              ▼         ▼
          VDD_APC   VDD_LPI_CX
          (variable) (0.7V)
```

---

## PS_HOLD Mechanism

`PS_HOLD` is a critical signal from the SoC to the PMIC:

```
SoC boots → Software sets PS_HOLD = HIGH → PMIC stays on

If PS_HOLD goes LOW:
  - PMIC kills all voltage rails
  - System powers off completely
  - Used for: shutdown, hard reset, watchdog timeout

PS_HOLD register: TCSR_RESET → triggers PMIC shutdown
```

### How PS_HOLD is Set

1. **PBL sets PS_HOLD early** — within first few instructions from ROM
2. **PS_HOLD stays asserted** through XBL → ABL → Kernel
3. **On shutdown**: kernel writes to `TCSR_RESET` register → PS_HOLD drops → PMIC shuts down

### PS_HOLD Timeout

If PBL fails to set PS_HOLD within the PMIC timeout (~8 seconds from power key press), the PMIC automatically shuts down all rails — this is a safety mechanism to prevent battery drain from a failed boot.

---

## Power-On Reason Register

The PMIC records **why** the system powered on:

```bash
# Read PON reason from PMIC (via sysfs or SPMI debug)
adb shell cat /sys/module/qpnp_power_on/parameters/pon_reason

# PON Reason bits:
# Bit 0: KPDPWR (power key)
# Bit 1: CBLPWR (cable/USB)
# Bit 2: PON1 (external trigger)
# Bit 3: USB_CHG
# Bit 4: DC_CHG
# Bit 5: RTC (alarm)
# Bit 6: SMPL (Sudden Momentary Power Loss)
# Bit 7: Hard Reset
```

### POFF (Power-Off) Reason

```bash
# Why did the system power off?
adb shell cat /sys/module/qpnp_power_on/parameters/poff_reason

# POFF Reason bits:
# Bit 0: SOFT (software shutdown)
# Bit 1: PS_HOLD (PS_HOLD went low)
# Bit 2: PMIC_WD (PMIC watchdog timeout)
# Bit 3: KPDPWR_AND_RESIN (key combo reset)
# Bit 4: GP_FAULT (general purpose fault)
# Bit 5: TFT (thermal fault trip)
```

---

## Bring-Up Debugging: Power Issues

### Common Power-Up Failures

| Symptom | Likely Cause | Debug |
|---------|-------------|-------|
| Board completely dead | PMIC not receiving battery power | Check VBAT connection |
| PMIC powers on then off | PS_HOLD not asserted | Check PBL execution, XBL image |
| SoC stuck in reset | VDD_CX/VDD_MX sequencing wrong | Probe rails with scope |
| Random crashes | VDD_MX < VDD_CX violation | Check regulator DT config |
| Overheating | Short on power rail | Check PCB for solder bridges |

### Debugging with Oscilloscope

For power-up bring-up, probe these signals with a scope:

```
Channel 1: KPDPWR (power button)
Channel 2: VDD_CX (core logic)
Channel 3: VDD_APC (CPU power)
Channel 4: POR (Power-On Reset to SoC)

Trigger on KPDPWR rising edge, verify:
1. VDD_CX rises before VDD_APC
2. POR deasserts after all rails are stable
3. Total sequence < 5 ms
```

---

## Related Documents

- [02_PBL_Primary_Bootloader.md](02_PBL_Primary_Bootloader.md) — What happens after POR deasserts
- [../00_SDM660_Architecture/05_PMIC_PM660.md](../00_SDM660_Architecture/05_PMIC_PM660.md) — PMIC architecture
- [../02_XBL_Secondary_Bootloader/04_PMIC_Regulator_Init.md](../02_XBL_Secondary_Bootloader/04_PMIC_Regulator_Init.md) — XBL fine-tunes PMIC config
