# RPM (Resource Power Manager) — Overview

## Overview

The **RPM (Resource Power Manager)** is a dedicated co-processor inside the SDM660 SoC that handles **power and clock management** independently of the main application processor. This allows the Apps processor to enter deep sleep while the RPM keeps essential subsystems running and responds to wake-up events.

---

## RPM Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                        SDM660 SoC                             │
│                                                              │
│  ┌──────────────┐              ┌──────────────────┐          │
│  │ Application  │              │    RPM            │          │
│  │ Processor    │◀── SMEM ───▶│    Co-Processor   │          │
│  │ (Cortex-A73/ │   (DDR)     │                   │          │
│  │  Cortex-A53) │              │  ┌─────────────┐ │          │
│  │              │              │  │ Cortex-M3   │ │          │
│  │ Runs Linux   │              │  │ (ARM)       │ │          │
│  │ + Android    │              │  │ ~200 MHz    │ │          │
│  └──────────────┘              │  └─────────────┘ │          │
│                                │                   │          │
│  ┌──────────────┐              │  ┌─────────────┐ │          │
│  │ Modem DSP    │◀── SMEM ───▶│  │ RPM RAM     │ │          │
│  └──────────────┘              │  │ ~160 KB     │ │          │
│                                │  └─────────────┘ │          │
│  ┌──────────────┐              │                   │          │
│  │ ADSP/CDSP    │◀── SMEM ───▶│  Controls:        │          │
│  └──────────────┘              │  • All clock PLLs │          │
│                                │  • Voltage rails  │          │
│                                │  • Bus bandwidth  │          │
│                                │  • Sleep modes    │          │
│                                └──────────────────┘          │
│                                                              │
│  ┌──────────────────────────────────────────────────┐        │
│  │              RPM Message RAM                      │        │
│  │              @ 0x0077_8000                         │        │
│  │  Apps ↔ RPM message exchange region               │        │
│  └──────────────────────────────────────────────────┘        │
└──────────────────────────────────────────────────────────────┘
```

---

## RPM Responsibilities

| Function | Description |
|----------|-------------|
| **Clock management** | Enable/disable/scale all GCC clocks on request |
| **Voltage regulation** | Aggregate regulator votes from all processors, set optimal voltage |
| **Bus bandwidth** | Aggregate NoC bandwidth votes, set bus clock frequencies |
| **Sleep management** | Handle Apps processor sleep entry/exit, coordinate with all subsystems |
| **Thermal protection** | Emergency frequency/voltage throttling on overheating |
| **Resource aggregation** | Multiple processors can request same resource; RPM picks optimal setting |

---

## RPM Firmware Loading

RPM firmware is loaded by XBL from the `rpm` partition:

```
XBL:
  1. Read rpm partition from flash
  2. Authenticate RPM firmware (signature verification via TZ)
  3. Load firmware to RPM RAM (~160 KB SRAM)
  4. Release RPM from reset
  5. RPM starts executing its firmware
  6. Handshake with Apps processor via SMEM
```

---

## Resource Voting Model

RPM uses a **voting model** — multiple clients (Apps, Modem, ADSP) can "vote" for a resource, and RPM picks the setting that satisfies all voters:

```
Example: VDD_CX Voltage Voting
─────────────────────────────────

Apps Processor:  "I need VDD_CX ≥ 0.872V" (Nominal)
Modem:           "I need VDD_CX ≥ 0.800V" (SVS)
ADSP:            "I need VDD_CX ≥ 0.644V" (Low SVS)

RPM Aggregation: MAX(0.872, 0.800, 0.644) = 0.872V
RPM sets VDD_CX = 0.872V

When Apps enters sleep:
Apps:            vote removed (or lowered to retention)
RPM Aggregation: MAX(0.800, 0.644) = 0.800V
RPM lowers VDD_CX = 0.800V → saves power
```

### Vote Types

| Vote Type | Meaning |
|-----------|---------|
| **Active** | Resource needed while processor is awake |
| **Sleep** | Minimum resource level during processor sleep |

---

## RPM Sleep Management

When the Apps processor enters deep sleep, RPM orchestrates the entire platform:

```
Apps Processor Sleep Entry:
──────────────────────────
1. Linux PM suspends all devices
2. Last CPU enters WFI (Wait-For-Interrupt)
3. PSCI calls TZ for power collapse
4. TZ signals RPM: "Apps going to sleep"
5. RPM receives Apps sleep votes
6. RPM aggregates all votes (Apps sleep + Modem + ADSP)
7. RPM lowers clocks/voltages to minimum needed
8. If ALL processors sleeping → RPM enters XO shutdown
   (only 32 KHz sleep clock remains)

Wake-up:
1. Interrupt arrives (timer, GPIO, modem)
2. RPM wakes up (if XO was shutdown, restores XO)
3. RPM restores clocks/voltages to active levels
4. RPM signals TZ to restore Apps processor
5. TZ restores CPU context
6. Linux PM resumes devices
```

---

## RPM in Device Tree

```dts
&rpm_bus {
    /* RPM-managed regulators */
    rpm-regulator-smpa1 {
        status = "okay";
        pm660_s1: regulator-s1 {
            regulator-name = "pm660_s1";
            qcom,set = <3>;  /* Both active and sleep sets */
            regulator-min-microvolt = <480000>;
            regulator-max-microvolt = <1230000>;
        };
    };

    /* RPM-managed clocks */
    rpm-clk-controller {
        compatible = "qcom,rpmcc-sdm660";
        #clock-cells = <1>;
    };
};
```

---

## RPM Debug

```bash
# RPM log (if enabled)
adb shell cat /sys/kernel/debug/rpm_log

# RPM stats (sleep/wake)
adb shell cat /sys/kernel/debug/rpm_stats

# RPM master stats
adb shell cat /sys/kernel/debug/rpm_master_stats

# Example output:
# RPM Mode:vlow  Count:142  Sleep(ticks):48293847
# Apps:   Sleep Count:89   Last: 2.3s ago
# Modem:  Sleep Count:45   Last: 0.1s ago
```

---

## Related Documents

- [02_RPM_Communication.md](02_RPM_Communication.md) — RPM messaging protocol
- [../02_XBL_Secondary_Bootloader/01_XBL_Overview.md](../02_XBL_Secondary_Bootloader/01_XBL_Overview.md) — XBL loads RPM firmware
- [../08_IPC_Mechanisms/01_SMEM_Shared_Memory.md](../08_IPC_Mechanisms/01_SMEM_Shared_Memory.md) — SMEM used for RPM communication
