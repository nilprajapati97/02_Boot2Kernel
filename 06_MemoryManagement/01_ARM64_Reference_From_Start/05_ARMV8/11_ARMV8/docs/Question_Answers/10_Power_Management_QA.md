# Power Management — Questions & Answers

---

## Q1. [L1] What are the ARM CPU power states? Explain the power domain hierarchy.

**Answer:**

```
ARM defines multiple power states for progressive power savings:

  ┌──────────────────────────────────────────────────────────┐
  │ CPU Power States (from least to most power saving):     │
  │                                                          │
  │ 1. Run:                                                  │
  │    CPU fully powered, executing instructions             │
  │    Dynamic power consumption: 100%                       │
  │                                                          │
  │ 2. Standby (WFI/WFE):                                   │
  │    CPU clock gated, state preserved                     │
  │    Dynamic power: ~0%, Static (leakage): 100%           │
  │    Wake-up: instant (< 1 μs)                            │
  │                                                          │
  │ 3. Retention:                                            │
  │    CPU powered down, state saved to retention cells     │
  │    Dynamic: 0%, Leakage: ~20% (retention cells powered) │
  │    Wake-up: fast (~10-50 μs)                            │
  │    Saves: GPRs, system registers in retention flip-flops │
  │                                                          │
  │ 4. Power-off (Powerdown):                               │
  │    CPU completely powered off, ALL state lost            │
  │    Dynamic: 0%, Leakage: 0%                             │
  │    Wake-up: slow (~100-1000 μs, like cold boot)         │
  │    Must save/restore all state to/from RAM              │
  │    L1/L2 cache lost → must clean before powerdown       │
  └──────────────────────────────────────────────────────────┘

Power domain hierarchy:
  ┌──────────────────────────────────────────────────────────┐
  │                                                          │
  │  ┌─────────────────────────────────────────────────┐    │
  │  │  System Power Domain (always-on)                 │    │
  │  │                                                   │    │
  │  │  ┌────────────────────────────────────────────┐  │    │
  │  │  │  Cluster Power Domain                      │  │    │
  │  │  │  (L2 cache, SCU, cluster logic)            │  │    │
  │  │  │                                            │  │    │
  │  │  │  ┌───────────┐  ┌───────────┐             │  │    │
  │  │  │  │ CPU Core 0│  │ CPU Core 1│             │  │    │
  │  │  │  │ Power     │  │ Power     │             │  │    │
  │  │  │  │ Domain    │  │ Domain    │             │  │    │
  │  │  │  │ (L1, regs)│  │ (L1, regs)│             │  │    │
  │  │  │  └───────────┘  └───────────┘             │  │    │
  │  │  │                                            │  │    │
  │  │  └────────────────────────────────────────────┘  │    │
  │  │                                                   │    │
  │  │  Rule: cluster can only power off if ALL cores   │    │
  │  │        in the cluster are powered off             │    │
  │  └─────────────────────────────────────────────────┘    │
  └──────────────────────────────────────────────────────────┘

Cache behavior during power transitions:
  Before core powerdown:
    1. Clean L1 data cache (DC CISW, set/way)
    2. Invalidate L1 instruction cache
    3. DSB SY (ensure completion)
    
  Before cluster powerdown:
    1. All cores: clean & invalidate L1
    2. Last core: clean & invalidate L2
    3. Disable coherency (CPUECTLR.SMPEN=0)
    4. DSB SY
```

---

## Q2. [L2] What is PSCI (Power State Coordination Interface)? How does Linux use it?

**Answer:**

```
PSCI is ARM's standard interface for power management operations,
called via SMC/HVC from OS to firmware (TF-A).

PSCI functions:
  ┌──────────────────────────────────────────────────────────┐
  │ Function          │ SMC ID       │ Purpose               │
  ├───────────────────┼──────────────┼───────────────────────┤
  │ CPU_SUSPEND       │ 0xC4000001  │ Enter low-power state │
  │ CPU_OFF           │ 0x84000002  │ Power off calling CPU │
  │ CPU_ON            │ 0xC4000003  │ Power on target CPU   │
  │ AFFINITY_INFO     │ 0xC4000004  │ Query CPU power state │
  │ SYSTEM_OFF        │ 0x84000008  │ System poweroff       │
  │ SYSTEM_RESET      │ 0x84000009  │ System reset          │
  │ SYSTEM_RESET2     │ 0xC4000012  │ Extended reset types  │
  │ SYSTEM_SUSPEND    │ 0xC400000E  │ Suspend to RAM        │
  │ PSCI_FEATURES     │ 0x8400000A  │ Query feature support │
  │ CPU_FREEZE        │ 0x84000007  │ Deeper than CPU_OFF  │
  │ PSCI_VERSION      │ 0x84000000  │ PSCI version (1.1)    │
  └───────────────────┴──────────────┴───────────────────────┘

CPU_ON flow (bringing up secondary CPUs at boot):
  ┌──────────────────────────────────────────────────────────┐
  │ Linux kernel on CPU 0:                                  │
  │   1. psci_cpu_on(cpu=2, entry=secondary_startup)       │
  │   2. SMC #0 with:                                       │
  │      X0 = 0xC4000003 (CPU_ON)                          │
  │      X1 = 0x00000002 (target MPIDR, Core 2)            │
  │      X2 = secondary_startup (entry point PA)           │
  │      X3 = 0 (context ID)                                │
  │                                                          │
  │ TF-A (EL3):                                             │
  │   3. Power on Core 2's power domain (via PMIC/SCP)     │
  │   4. Set Core 2's reset vector to entry point          │
  │   5. Release Core 2 from reset                         │
  │   6. Return PSCI_SUCCESS (0) to caller                 │
  │                                                          │
  │ Core 2 wakes up:                                        │
  │   7. Starts executing at secondary_startup              │
  │   8. Initializes its EL1 state (MMU, caches)           │
  │   9. Calls cpu_startup_entry() → enters idle loop      │
  └──────────────────────────────────────────────────────────┘

CPU_SUSPEND flow (cpuidle):
  Linux CPUidle governor selects idle state:
    1. cpuidle_enter_state() → psci_cpu_suspend()
    2. Save CPU context (registers, system registers)
    3. Clean caches (if cluster powerdown)
    4. SMC: CPU_SUSPEND(power_state, entry_point)
    
  power_state parameter:
    Bits [3:0]:   StateID (platform-specific)
    Bit [16]:     StateType: 0=standby, 1=powerdown
    Bits [25:24]: PowerLevel: 0=core, 1=cluster, 2=system
    
  TF-A handles actual power sequencing:
    Core powerdown: disable caches, gate clocks, cut power
    Core wake-up: power on, reset, jump to entry point
    
  Linux: resume at entry_point → restore context → return
```

---

## Q3. [L2] How does DVFS (Dynamic Voltage and Frequency Scaling) work on ARM?

**Answer:**

```
DVFS adjusts CPU voltage and frequency dynamically to balance
performance and power consumption.

Power equation:
  P_dynamic = α × C × V² × f
    α = activity factor (% of transistors switching)
    C = capacitance (fixed by chip design)
    V = supply voltage
    f = clock frequency
    
  Key insight: reducing V by 20% → power drops by 36% (V²)
  But: lower V → lower maximum stable frequency
  → V and f must scale together (OPP = Operating Performance Point)

OPP table example (Cortex-A76):
  ┌──────────────────────────────────────────────────────────┐
  │ OPP     │ Frequency  │ Voltage  │ Relative Power       │
  ├─────────┼────────────┼──────────┼──────────────────────┤
  │ OPP 1   │ 600 MHz    │ 0.65V    │ 100% (baseline)      │
  │ OPP 2   │ 1000 MHz   │ 0.75V    │ 280%                 │
  │ OPP 3   │ 1500 MHz   │ 0.85V    │ 570%                 │
  │ OPP 4   │ 2000 MHz   │ 0.95V    │ 950%                 │
  │ OPP 5   │ 2400 MHz   │ 1.05V    │ 1500%                │
  │ OPP 6   │ 2800 MHz   │ 1.15V    │ 2200%                │
  └─────────┴────────────┴──────────┴──────────────────────┘
  
  → Going from 600 MHz to 2800 MHz: 4.7x frequency
    but 22x power consumption! (due to V²×f)

Linux cpufreq architecture:
  ┌──────────────────────────────────────────────────────────┐
  │ Userspace:                                               │
  │   /sys/devices/system/cpu/cpu0/cpufreq/                 │
  │   scaling_governor: schedutil / performance / powersave │
  │   scaling_cur_freq: current frequency                   │
  │   scaling_available_frequencies: list of OPPs           │
  │                                                          │
  │ Kernel:                                                  │
  │   Governor (policy decision):                           │
  │     schedutil: frequency ∝ CPU utilization              │
  │     performance: always max frequency                   │
  │     powersave: always min frequency                     │
  │     ondemand: load-based (legacy)                       │
  │                                                          │
  │   Driver (hardware control):                            │
  │     cpufreq-dt: generic device-tree based               │
  │     scmi-cpufreq: via SCMI to SCP firmware              │
  │     → Programs PLL (frequency) and PMIC (voltage)       │
  └──────────────────────────────────────────────────────────┘

Transition sequence:
  To increase frequency:
    1. Raise voltage FIRST (via PMIC I2C/SPI)
    2. Wait for voltage to stabilize (~10-100 μs)
    3. Switch PLL to new frequency
    → Voltage must be high enough for new frequency!
  
  To decrease frequency:
    1. Switch PLL to new (lower) frequency FIRST
    2. Then reduce voltage
    → Ensure frequency is low enough for lower voltage!
```

---

## Q4. [L2] What is the SCP (System Control Processor)? How does it manage power?

**Answer:**

```
SCP is a dedicated microcontroller (typically Cortex-M) that
manages power, clocks, and thermals for the main application
processors.

SCP role:
  ┌──────────────────────────────────────────────────────────┐
  │                    SoC Architecture                      │
  │                                                          │
  │  ┌───────────────┐    ┌──────────────────────┐          │
  │  │ Application   │    │ SCP (Cortex-M3/M7)    │         │
  │  │ Processor     │    │                        │         │
  │  │ Cortex-A76    │◄──►│ • DVFS control        │         │
  │  │               │SCMI│ • Power domain mgmt   │         │
  │  │ Runs Linux    │    │ • Clock management    │         │
  │  │               │    │ • Thermal monitoring  │         │
  │  └───────────────┘    │ • Sensor reading      │         │
  │                        │ • PMIC programming    │         │
  │                        │ • Always-on domain    │         │
  │                        └──────────┬───────────┘         │
  │                                   │                      │
  │                        ┌──────────┴───────────┐         │
  │                        │   PMIC / Regulators   │         │
  │                        │   Clock generators    │         │
  │                        │   Temperature sensors │         │
  │                        └──────────────────────┘         │
  └──────────────────────────────────────────────────────────┘

Why SCP?
  1. Application CPUs can be powered off completely
     → SCP stays on to wake them when needed
  2. Power sequencing is complex (ordering, timing)
     → SCP handles all power domain transitions
  3. Real-time thermal management
     → SCP monitors temperature, throttles if needed
  4. Low-power always-on processing
     → SCP runs on μW power (vs mW-W for app CPUs)

SCMI (System Control and Management Interface):
  Standard protocol between Linux and SCP.
  
  Transport: shared memory + doorbell interrupt
  
  SCMI protocol domains:
    Power:       power domain on/off
    Performance: DVFS (set frequency/voltage)
    Clock:       clock enable/rate control
    Sensor:      read temperature, voltage, current
    Reset:       domain reset control
    Voltage:     voltage regulator control
    
  Linux driver: drivers/firmware/arm_scmi/
    scmi_perf_ops: set_performance_level()
    scmi_power_ops: power_state_set()
    scmi_sensor_ops: reading_get()

  Message flow:
    1. Linux: write SCMI message to shared memory
    2. Linux: ring doorbell (write SCMI doorbell register)
    3. SCP: reads shared memory, processes command
    4. SCP: programs PMIC/PLL/clock hardware
    5. SCP: writes response to shared memory
    6. SCP: triggers completion interrupt to Linux
    7. Linux: reads response
```

---

## Q5. [L2] How does Linux CPUidle work on ARM64? What are idle states?

**Answer:**

```
CPUidle framework selects the optimal power state when a CPU
core has no work to do.

Idle states on ARM64 (from device tree + PSCI):
  ┌──────────────────────────────────────────────────────────┐
  │ State        │ Exit     │ Power    │ What's saved       │
  │              │ Latency  │ Savings  │                    │
  ├──────────────┼──────────┼──────────┼────────────────────┤
  │ WFI          │ <1 μs    │ Low      │ Everything         │
  │ (standby)    │          │          │ (clock gated only) │
  ├──────────────┼──────────┼──────────┼────────────────────┤
  │ Core         │ ~20 μs   │ Medium   │ State in retention │
  │ Retention    │          │          │ cells              │
  ├──────────────┼──────────┼──────────┼────────────────────┤
  │ Core         │ ~200 μs  │ High     │ Nothing (must save │
  │ Powerdown    │          │          │ to RAM, cold boot) │
  ├──────────────┼──────────┼──────────┼────────────────────┤
  │ Cluster      │ ~500 μs  │ Very High│ L2 cache lost,     │
  │ Powerdown    │          │          │ cluster logic off  │
  └──────────────┴──────────┴──────────┴────────────────────┘

CPUidle governor decision:
  ┌──────────────────────────────────────────────────────────┐
  │ Governor looks at:                                      │
  │   • Expected idle duration (predicted from history)    │
  │   • Exit latency of each state                         │
  │   • Target residency (minimum time to be worthwhile)   │
  │                                                          │
  │ TEO (Timer Events Oriented) governor:                   │
  │   Predicts next wake-up from timer events              │
  │   If predicted idle > target_residency[state]:         │
  │     → Enter that state                                  │
  │   Else:                                                  │
  │     → Choose shallower state                            │
  │                                                          │
  │ Example decision:                                       │
  │   Next timer in 50 μs:                                  │
  │     Core Powerdown (target=100 μs) → too deep!         │
  │     Core Retention (target=10 μs) → good! Use this     │
  │                                                          │
  │   Next timer in 5 ms:                                    │
  │     Cluster Powerdown (target=300 μs) → plenty of time │
  │     → Use deepest state, maximum power savings         │
  └──────────────────────────────────────────────────────────┘

Device tree idle states:
  cpu_idle_states {
      entry-method = "psci";    // Use PSCI for power control
      
      CPU_SLEEP: cpu-sleep {
          compatible = "arm,idle-state";
          arm,psci-suspend-param = <0x0010000>;  // Core powerdown
          entry-latency-us = <40>;    // Time to enter state
          exit-latency-us = <200>;    // Time to wake up
          min-residency-us = <800>;   // Minimum time to be worth it
          local-timer-stop;           // Timer stops in this state
      };
      
      CLUSTER_SLEEP: cluster-sleep {
          compatible = "arm,idle-state";
          arm,psci-suspend-param = <0x1010000>;  // Cluster powerdown
          entry-latency-us = <200>;
          exit-latency-us = <500>;
          min-residency-us = <2000>;
          local-timer-stop;
      };
  };
```

---

## Q6. [L3] How does system suspend (Suspend-to-RAM) work on ARM64?

**Answer:**

```
Suspend-to-RAM powers off most of the SoC, keeping only DDR
in self-refresh and a small always-on domain.

Suspend flow:
  ┌──────────────────────────────────────────────────────────┐
  │ 1. Userspace: echo mem > /sys/power/state               │
  │                                                          │
  │ 2. PM framework: freeze all user processes              │
  │    → Send SIGSTOP to all tasks                          │
  │    → Drain workqueues                                   │
  │                                                          │
  │ 3. Device suspend (reverse registration order):         │
  │    → Each driver: save device state, disable interrupts │
  │    → DMA stopped, clocks gated, regulators off          │
  │    → Example: UART: save registers, disable, gate clock │
  │                                                          │
  │ 4. Disable non-boot CPUs:                               │
  │    → PSCI CPU_OFF for each secondary core               │
  │    → Only CPU 0 remains running                         │
  │                                                          │
  │ 5. System suspend (last CPU):                           │
  │    → syscore_suspend(): GIC, timer, arch-specific save  │
  │    → Save CPU 0 state to RAM (registers, system regs)  │
  │    → Clean all caches (DC CISW)                         │
  │    → PSCI SYSTEM_SUSPEND (SMC)                          │
  │                                                          │
  │ 6. TF-A (EL3):                                          │
  │    → Power off CPU cluster(s)                           │
  │    → Power off interconnect                             │
  │    → Switch DDR to self-refresh                         │
  │    → Clock-gate / power-off most of SoC                │
  │    → Only alive: PMIC, RTC, wake-up controller          │
  │                                                          │
  │ ======= SYSTEM SLEEPING (μW power consumption) =======  │
  │                                                          │
  │ 7. Wake event! (power button, RTC alarm, GPIO)          │
  │    → Wake-up controller triggers PMIC                   │
  │    → Power restored to SoC                              │
  │    → CPU resets (like cold boot)                        │
  │                                                          │
  │ 8. TF-A (BL1 → BL31):                                  │
  │    → Normal boot up to BL31                             │
  │    → Detect: this is resume, not cold boot              │
  │    → DDR already initialized (self-refresh preserved)  │
  │    → Restore CPU state from RAM                        │
  │    → ERET to Linux resume entry point                   │
  │                                                          │
  │ 9. Linux resume:                                        │
  │    → Restore MMU, caches, GIC state                    │
  │    → syscore_resume()                                   │
  │    → Power on secondary CPUs (PSCI CPU_ON)             │
  │    → Device resume (forward order): restore all devices│
  │    → Thaw processes                                    │
  │    → System running again!                             │
  └──────────────────────────────────────────────────────────┘

Wake sources:
  Configured in always-on domain:
    • GPIO wake-up controller (power button)
    • RTC alarm (timed wake)
    • USB VBUS detect (charger insertion)
    • Network card Wake-on-LAN (if supported)
    
  Each wake source must be connected to always-on logic
  that can trigger power restore without CPU involvement.
```

---

Back to [Question & Answers Index](./README.md)
