# DVFS — Dynamic Voltage and Frequency Scaling

## 1. What Is DVFS?

DVFS adjusts CPU **voltage and frequency** at runtime to balance performance vs power consumption.

```
Fundamental relationship:
  
  Dynamic Power ∝ C × V² × f
  
  Where:
    C = capacitance (fixed by chip design)
    V = supply voltage
    f = clock frequency

  Reducing voltage by 2x → power reduces by 4x (squared!)
  Reducing frequency by 2x → power reduces by 2x
  
  But: lower voltage requires lower frequency (slower switching)
  → Voltage and frequency are always scaled TOGETHER

Example (Cortex-A76):
  ┌──────────────┬──────────┬──────────┬──────────────────┐
  │ OPP          │ Frequency│ Voltage  │ Relative Power   │
  ├──────────────┼──────────┼──────────┼──────────────────┤
  │ Performance  │ 2.8 GHz  │ 1.05 V   │ 100% (max)      │
  │ Nominal      │ 2.4 GHz  │ 0.90 V   │ ~56%            │
  │ Low          │ 1.8 GHz  │ 0.75 V   │ ~26%            │
  │ Ultra-Low    │ 1.0 GHz  │ 0.60 V   │ ~8%             │
  │ Min          │ 0.5 GHz  │ 0.50 V   │ ~2%             │
  └──────────────┴──────────┴──────────┴──────────────────┘
  
  OPP = Operating Performance Point
```

---

## 2. big.LITTLE / DynamIQ Heterogeneous Computing

```
ARM SoCs use heterogeneous CPU clusters for power efficiency:

  big.LITTLE (ARMv8):
  ┌──────────────────────────────────┐
  │  big Cluster (high performance)  │
  │  ┌──────┐ ┌──────┐              │
  │  │A76/78│ │A76/78│  2.8 GHz     │
  │  │(big) │ │(big) │  High power  │
  │  └──────┘ └──────┘              │
  └──────────────────────────────────┘
  ┌──────────────────────────────────┐
  │  LITTLE Cluster (efficiency)     │
  │  ┌──────┐ ┌──────┐ ┌──────┐    │
  │  │ A55  │ │ A55  │ │ A55  │    │
  │  │(LITTLE│ │(LITTLE│ │(LITTLE   │
  │  └──────┘ └──────┘ └──────┘    │
  │  1.8 GHz, Low power             │
  └──────────────────────────────────┘

  DynamIQ (ARMv8.2+):
  ┌──────────────────────────────────────────┐
  │  Single DynamIQ Cluster (DSU)            │
  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐   │
  │  │ X3   │ │ A720 │ │ A520 │ │ A520 │   │
  │  │(prime)│ │(big) │ │(LITTLE│ │(LITTLE│  │
  │  │3.3GHz│ │2.8GHz│ │2.0GHz│ │2.0GHz│   │
  │  └──────┘ └──────┘ └──────┘ └──────┘   │
  │  Shared L3 Cache via DSU                 │
  └──────────────────────────────────────────┘
  
  DynamIQ advantage: Mixed core types in ONE cluster
  → Faster migration between big/LITTLE (shared L3)
  → More flexible configurations (1big+3LITTLE, etc.)

Linux scheduler:
  • Light tasks (email, messaging) → LITTLE cores
  • Heavy tasks (gaming, compilation) → big cores
  • Task migration based on load (Energy-Aware Scheduling — EAS)
```

---

## 3. CPUfreq Governors (Linux)

```
Linux CPUfreq framework manages DVFS:

  $ cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
  performance schedutil powersave ondemand conservative

Governors:
┌───────────────┬──────────────────────────────────────────────────┐
│ Governor      │ Behavior                                         │
├───────────────┼──────────────────────────────────────────────────┤
│ performance   │ Always max frequency                             │
│ powersave     │ Always min frequency                             │
│ ondemand      │ Jump to max on load, reduce when idle            │
│ conservative  │ Step up/down gradual                             │
│ schedutil     │ Uses scheduler utilization data (preferred!)     │
│               │ Tightly integrated with CFS scheduler            │
│               │ Reacts within 1 scheduler tick (~4ms)            │
└───────────────┴──────────────────────────────────────────────────┘

schedutil flow:
  1. CFS scheduler computes per-CPU utilization (0-1024)
  2. schedutil maps utilization to OPP
  3. Driver programs SCMI/clock controller
  4. Hardware changes voltage+frequency

SCMI (System Control and Management Interface):
  Standard protocol for OS ↔ SCP (System Control Processor)
  OS: "set CPU0 cluster to 2.4 GHz"
  SCP: Programs voltage regulator + PLL
  
  SCP is a tiny embedded microcontroller (Cortex-M) on the SoC
  that controls power rails, clocks, and thermal management.
```

---

## 4. AMU — Activity Monitors (ARMv8.4)

```
AMU provides ALWAYS-ON hardware counters for power management:

  Unlike PMU counters (which can be disabled/context-switched),
  AMU counters run continuously — even across idle states.

  AMU counters:
  ┌────────────────────┬──────────────────────────────────────────┐
  │ Counter            │ Purpose                                   │
  ├────────────────────┼──────────────────────────────────────────┤
  │ AMEVCNTR0_CORE_EL0│ Core cycles (actual frequency)           │
  │ AMEVCNTR0_CONST_EL0│ Constant frequency reference (fixed)    │
  │ AMEVCNTR0_INST_EL0│ Instructions retired                     │
  │ AMEVCNTR0_MEM_EL0 │ Memory stall cycles                      │
  └────────────────────┴──────────────────────────────────────────┘

  Use: Calculate ACTUAL CPU frequency delivered
    actual_freq = (core_cycles / const_cycles) × const_freq
    
  This tells the OS: "the DVFS decision actually delivered
  this much performance" — enabling better frequency selection.
  
  Linux: cppc_cpufreq driver uses AMU for frequency feedback
```

---

## 5. Thermal Management

```
ARM cores include thermal sensors and throttling:

  ┌──────────────────────────────────────────────────────────────┐
  │  Temperature zones:                                           │
  │                                                                │
  │  ┌─────────────────────────────────────────────┐              │
  │  │ 100°C ████████████████████████████ CRITICAL │ → Shutdown  │
  │  │  95°C ██████████████████████████ PASSIVE    │ → Max throt │
  │  │  85°C ████████████████████ HOT              │ → DVFS down │
  │  │  70°C ██████████████ WARM                   │ → Normal    │
  │  │  40°C ████████ NORMAL                       │ → Full perf │
  │  │  25°C ████ COLD                             │ → Full perf │
  │  └─────────────────────────────────────────────┘              │
  │                                                                │
  │  Linux thermal framework:                                     │
  │  /sys/class/thermal/thermal_zone0/                            │
  │    temp        → current temperature (millidegrees)           │
  │    trip_point_0_temp → passive throttling threshold           │
  │    trip_point_1_temp → critical shutdown threshold            │
  │                                                                │
  │  Cooling devices:                                              │
  │  • cpu_cooling: reduce CPU frequency (passive cooling)        │
  │  • gpu_cooling: reduce GPU frequency                          │
  │  • fan: increase fan speed (active cooling)                   │
  │                                                                │
  │  IPA (Intelligent Power Allocation):                          │
  │  • Budget-based thermal management                            │
  │  • Distributes power budget between CPU and GPU               │
  │  • PID controller adjusts allocations to maintain target temp│
  └──────────────────────────────────────────────────────────────┘
```

---

Next: [Power States →](./02_Power_States.md) | Back to [Power Management Overview](./README.md)
