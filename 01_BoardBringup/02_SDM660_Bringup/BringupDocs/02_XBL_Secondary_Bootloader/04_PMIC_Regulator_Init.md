# PMIC Regulator Initialization in XBL

## Overview

After PBL performs basic PMIC power-up (fixed sequencing from PMIC hardware), XBL **fine-tunes** the voltage regulators via SPMI commands. This includes adjusting voltage levels based on the specific board configuration, enabling additional LDOs, and setting up regulator modes for optimal efficiency.

---

## What XBL Configures on PMIC

```
XBL PMIC Init:
──────────────
1. Verify PMIC presence and revision
   ├── Read PM660 REVID register via SPMI
   └── Read PM660L REVID register via SPMI

2. Configure VDD_CX (Core Logic)
   ├── Read CPR (Core Power Reduction) fuse values
   ├── Adjust VDD_CX voltage based on silicon corner
   └── S1 set to 0.800V - 0.872V (depends on part)

3. Configure VDD_MX (Memory)
   ├── Must be ≥ VDD_CX at all times
   └── S2 set to 1.05V

4. Configure VDD_APC (CPU)
   ├── Read CPU speed-bin fuses
   ├── Set initial voltage for boot frequency
   ├── Gold cluster: S3 → ~0.9V (for 300 MHz boot freq)
   └── Silver cluster: S6 → ~0.8V

5. Configure DDR voltage
   └── S5 set to 1.35V (LPDDR4) or adjusted for LPDDR4x

6. Enable I/O LDOs
   ├── L9 (1.8V) — I2C/SPI I/O voltage
   ├── L11 (1.8V) — USB PHY
   └── L12 (1.8V) — DSI display I/O

7. Set regulator modes
   ├── High-current rails: PWM mode (best for fast transients)
   └── Low-current rails: AUTO mode (PFM/PWM switching for efficiency)

8. Configure PMIC watchdog
   └── If SoC stops communicating, PMIC can trigger reset
```

---

## Silicon Corner & CPR (Core Power Reduction)

Not all SDM660 chips are identical — manufacturing variations mean some chips are "faster" (need less voltage) and some are "slower" (need more voltage). This is encoded in **fuses** read by XBL.

```
Silicon Speed Bins:
───────────────────
┌────────────┬──────────────┬──────────────┐
│  Corner    │  VDD_CX (V)  │  CPU Max Freq│
├────────────┼──────────────┼──────────────┤
│  SVS       │  0.800       │  Lower       │
│  Nominal   │  0.872       │  Mid         │
│  Turbo     │  0.952       │  High        │
│  Super     │  1.040       │  Max (2.2G)  │
└────────────┴──────────────┴──────────────┘

XBL reads fuse → determines corner → sets initial voltage
Kernel CPR driver later does dynamic voltage scaling
```

---

## SPMI Register Writes (How XBL Talks to PMIC)

```
SPMI Write Flow:
────────────────
CPU (XBL code)
    │
    ▼
SPMI Master Controller (in SoC)
    │
    ├── SID = 0 (PM660)
    ├── PID = peripheral base
    ├── Register offset
    └── Data byte
    │
    ▼ (2-wire SPMI bus: CLK + DATA)
    │
PM660 PMIC
    │
    └── Register updated (e.g., voltage set)
```

### Example: Setting PM660 S1 (VDD_CX) to 0.872V

```
SPMI Write:
  SID: 0x0 (PM660)
  Address: 0x1440 + 0x40 (S1 VSET_LB register)
  Data: voltage_code = (0.872 - 0.300) / 0.004 = 143 = 0x8F

  Register: PM660_S1_CTRL_VSET_LB = 0x8F
  Register: PM660_S1_CTRL_VSET_UB = 0x00

Result: S1 regulator outputs 0.872V
```

---

## Regulator Modes

Each SMPS (buck) regulator can operate in different modes:

| Mode | Description | Efficiency | Ripple | Use Case |
|------|-------------|------------|--------|----------|
| PWM | Pulse Width Modulation | Good at high load | Higher | CPU under load |
| PFM | Pulse Frequency Modulation | Excellent at low load | Lower | Sleep/idle |
| AUTO | Auto-switch PWM↔PFM | Best overall | Varies | Default for most rails |
| BYPASS | Regulator bypassed | N/A | N/A | When input ≈ output |

```
XBL sets mode for each regulator:
──────────────────────────────────
S1 (VDD_CX):  AUTO mode — switches based on load
S3 (VDD_APC): PWM mode  — fast transient response for CPU
S5 (VDD_DDR): PWM mode  — stable for DDR operations
L-series:      Normal mode (LDO, no mode selection)
```

---

## Voltage Rail Adjustments by Board

Different boards with the same SDM660 SoC may need different PMIC configurations based on:

| Factor | Impact |
|--------|--------|
| DDR vendor | Different LPDDR4 chips need different VDD_DDR |
| Display panel | Different panels need different DSI I/O voltage |
| Sensor config | Different sensors need different LDO voltages |
| Board revision | PCB changes may affect power integrity |

XBL reads the **board ID** from strapping resistors or fuses and applies board-specific PMIC configuration from the CDT in `xbl_config`.

---

## PMIC Configuration in Device Tree (for Kernel)

After XBL initializes PMICs, the kernel's regulator driver takes over:

```dts
&pm660_regulators {
    S1: pm660_s1: regulator-s1 {
        regulator-name = "pm660_s1";
        qcom,set = <RPMH_REGULATOR_SET_ACTIVE>;
        regulator-min-microvolt = <480000>;
        regulator-max-microvolt = <1230000>;
        qcom,init-voltage = <872000>;  /* XBL sets this initially */
    };

    L13: pm660_l13: regulator-l13 {
        regulator-name = "pm660_l13";
        regulator-min-microvolt = <1780000>;
        regulator-max-microvolt = <1950000>;
        qcom,init-voltage = <1800000>;
        /* Sensor VDD — used by BMI160 */
    };
};
```

---

## Debugging PMIC Issues in XBL

### XBL PMIC Log Messages

```
[PMIC] PM660 Detected: Rev 2.0
[PMIC] PM660L Detected: Rev 2.0
[PMIC] S1 (VDD_CX): Set to 872 mV, MODE=AUTO
[PMIC] S2 (VDD_MX): Set to 1050 mV, MODE=AUTO
[PMIC] S3 (VDD_APC_Gold): Set to 900 mV, MODE=PWM
[PMIC] S5 (VDD_DDR): Set to 1350 mV, MODE=PWM
[PMIC] L9: Set to 1800 mV, Enabled
[PMIC] CPR Fuse: Corner=Nominal, Speed_Bin=2
```

### Common PMIC Issues

| Issue | Symptom | Fix |
|-------|---------|-----|
| Wrong PMIC revision | XBL crashes early | Update XBL for correct PMIC rev |
| VDD_CX too low | Random SoC hangs | Check CPR fuse reading, adjust floor voltage |
| LDO not enabled | Peripheral doesn't work | Enable in XBL or kernel DT |
| SPMI communication fail | XBL hangs at PMIC init | Check SPMI bus connections on PCB |

---

## Related Documents

- [01_XBL_Overview.md](01_XBL_Overview.md) — XBL overall flow
- [03_Clock_Tree_Setup.md](03_Clock_Tree_Setup.md) — Clocks need stable power
- [05_TrustZone_QSEE_Init.md](05_TrustZone_QSEE_Init.md) — TZ loaded after PMIC is stable
- [../00_SDM660_Architecture/05_PMIC_PM660.md](../00_SDM660_Architecture/05_PMIC_PM660.md) — PMIC architecture
- [../05_Linux_Kernel_Boot/06_Regulator_Framework.md](../05_Linux_Kernel_Boot/06_Regulator_Framework.md) — Kernel regulator driver
