# Power States

## 1. CPU Power States Overview

```
ARMv8 defines a hierarchy of power states:

  ┌─────────────────────────────────────────────────────────────┐
  │  Power State       │ CPU          │ Cache      │ Latency   │
  ├────────────────────┼──────────────┼────────────┼───────────┤
  │ Run                │ Active       │ Active     │ 0         │
  │ Standby (WFI)      │ Clock-gated  │ Active     │ ~1 µs     │
  │ Retention          │ Power-down   │ Retained   │ ~10 µs    │
  │                    │ (state saved)│ (tag RAM)  │           │
  │ Powerdown (core)   │ Power-off    │ Lost       │ ~100 µs   │
  │ Cluster powerdown  │ All cores off│ L2/L3 lost │ ~500 µs   │
  │ System suspend     │ Everything   │ Everything │ ~10 ms    │
  │                    │ off          │ Lost       │           │
  └────────────────────┴──────────────┴────────────┴───────────┘

  Deeper state = more power saving = longer wake-up latency
  OS must balance: how long will we idle vs wake-up cost?
```

---

## 2. WFI and WFE Instructions

```
WFI — Wait For Interrupt:
  • Puts core into low-power standby state
  • Core stops fetching/executing instructions
  • Clock to core pipeline is gated (stopped)
  • Wakes up on: IRQ, FIQ, SError, or debug event
  • Used by: idle loop, PSCI CPU_SUSPEND

  Linux idle loop:
    while (1) {
        // No runnable tasks
        local_irq_disable();
        // Select idle state (cpuidle governor)
        if (shallow_idle) {
            wfi();           // Simple clock gating
        } else {
            psci_cpu_suspend(); // Deep power-down via SMC
        }
        local_irq_enable();
        schedule();
    }

WFE — Wait For Event:
  • Similar to WFI but wakes on EVENT signal
  • Used for: spinlock optimization
  • Sends core to standby until another core signals
  
  Spinlock with WFE:
    spin_lock:
      LDAXR W0, [X1]         // Load-exclusive lock variable
      CBNZ  W0, spin_wait    // If locked, wait
      STXR  W2, W3, [X1]     // Try to acquire
      CBNZ  W2, spin_lock    // Retry if exclusive failed
      RET

    spin_wait:
      WFE                     // Sleep until event
      B     spin_lock         // Retry
    
    spin_unlock:
      STLR  WZR, [X1]        // Release lock
      SEV                     // Send Event to wake waiting cores

  Without WFE: spinning core burns power checking lock
  With WFE: spinning core sleeps, wakes only when lock released
```

---

## 3. PSCI — Power State Coordination Interface

```
PSCI is ARM's standard interface for CPU power management.
OS/hypervisor calls PSCI via SMC. TF-A (ATF) implements it.

PSCI Functions:
┌──────────────────────┬──────────────────────────────────────────┐
│ Function             │ Description                               │
├──────────────────────┼──────────────────────────────────────────┤
│ PSCI_VERSION         │ Returns PSCI version (e.g., 1.1)        │
│ CPU_ON               │ Power on a specific core                 │
│ CPU_OFF              │ Power off calling core                   │
│ CPU_SUSPEND          │ Put calling core into low-power state    │
│ AFFINITY_INFO        │ Query power state of an affinity level   │
│ SYSTEM_OFF           │ Shutdown entire system                   │
│ SYSTEM_RESET         │ Reset entire system                      │
│ SYSTEM_SUSPEND       │ Suspend entire system (deep sleep)      │
│ CPU_FREEZE           │ Park a core (for hotplug)               │
│ FEATURES             │ Query which PSCI features are supported  │
│ STAT_RESIDENCY       │ Time spent in a power state              │
│ STAT_COUNT           │ Number of times entered a power state    │
└──────────────────────┴──────────────────────────────────────────┘

PSCI calling convention:
  // Power on CPU core 2 (MPIDR 0.0.2)
  MOV X0, #0xC4000003     // PSCI_CPU_ON (SMC64)
  MOV X1, #0x002          // Target CPU MPIDR
  MOV X2, #entry_point    // Where the core starts executing
  MOV X3, #context_id     // Argument passed to the core
  SMC #0                   // Call Secure Monitor
  // X0 = return code: 0 = SUCCESS

  // Suspend calling core
  MOV X0, #0xC4000001     // PSCI_CPU_SUSPEND
  MOV X1, #power_state    // Desired power level
  MOV X2, #resume_addr    // Where to resume after wakeup
  MOV X3, #context        // Context ID
  SMC #0
```

---

## 4. CPU_SUSPEND Power State Encoding

```
PSCI_CPU_SUSPEND power_state parameter:
  ┌────────────────────────────────────────────────────────┐
  │ Bits [31:30]: reserved                                  │
  │ Bit  [30]:    StateType: 0=Standby, 1=PowerDown        │
  │ Bits [27:24]: PowerLevel:                               │
  │               0=Core, 1=Cluster, 2=System               │
  │ Bits [15:0]:  StateID (platform-specific)               │
  └────────────────────────────────────────────────────────┘

  Examples:
  ┌─────────────────────────────┬──────────────────────────┐
  │ Power State                 │ power_state value        │
  ├─────────────────────────────┼──────────────────────────┤
  │ Core standby (clock gate)   │ 0x0000_0000              │
  │ Core power-down             │ 0x4000_0001              │
  │ Cluster power-down          │ 0x4100_0002              │
  │ System suspend              │ 0x4200_0003              │
  └─────────────────────────────┴──────────────────────────┘
```

---

## 5. Linux CPUidle Framework

```
CPUidle selects the best idle state based on expected idle duration:

  cpuidle governors:
  ┌────────────────┬───────────────────────────────────────────┐
  │ Governor       │ Description                                │
  ├────────────────┼───────────────────────────────────────────┤
  │ menu           │ Predicts idle duration from timer events  │
  │                │ and I/O patterns. Default for tickless.   │
  │ teo            │ Timer Events Oriented — newer, looks at   │
  │                │ actual timer wakeup history               │
  │ ladder         │ Progressively deeper states. Simple.      │
  │ haltpoll       │ Poll briefly before entering idle (VMs)   │
  └────────────────┴───────────────────────────────────────────┘

  /sys/devices/system/cpu/cpu0/cpuidle/
  ├── state0/
  │   ├── name        → "WFI"
  │   ├── latency     → 1 (µs exit latency)
  │   ├── residency   → 1 (µs minimum time to be worthwhile)
  │   ├── power       → 100 (mW, estimated)
  │   └── usage       → 1234567 (times entered)
  ├── state1/
  │   ├── name        → "cpu-sleep"
  │   ├── latency     → 100 (µs)
  │   ├── residency   → 500 (µs)
  │   └── power       → 20 (mW)
  └── state2/
      ├── name        → "cluster-sleep"
      ├── latency     → 500 (µs)
      ├── residency   → 5000 (µs)
      └── power       → 0 (mW)

  Governor decision:
    predicted_idle_time = 2000 µs
    
    state0: residency 1 µs     → eligible, saves little power
    state1: residency 500 µs   → eligible, good power savings
    state2: residency 5000 µs  → NOT eligible (2000 < 5000)
    
    → Select state1 (cpu-sleep via PSCI CPU_SUSPEND)
```

---

## 6. Core/Cluster Power-Down Sequence

```
What happens when a core powers down:

  1. OS saves volatile state:
     • All general-purpose registers (X0-X30, SP)
     • System registers (SCTLR, TCR, TTBR, MAIR, etc.)
     • Floating-point registers (V0-V31, FPCR, FPSR)
     • GIC CPU Interface state
     • Debug/breakpoint registers
     • Timer state

  2. Clean and invalidate caches:
     DC CISW (clean+invalidate by set/way) for L1
     Or: PSCI implementation handles this

  3. Call PSCI CPU_SUSPEND via SMC:
     → EL3 (ATF Secure Monitor):
       - Saves EL3 registers
       - Programs power controller to gate clock/power
       - Executes WFI (last instruction before power-off)
       
  4. Power is cut:
     • Core clock stops
     • Optional: voltage reduced or rail cut
     • L1 cache contents LOST (unless retention)
     • Pipeline state LOST

  5. Wake-up (interrupt arrives):
     • Power restored, clock starts
     • Core resets (warm reset)
     • Enters BL31 warm boot path
     • BL31 restores EL3 state
     • ERET back to OS resume point
     • OS restores saved register context
     • Core is running again

Cluster power-down:
  Same as above but:
  • ALL cores in cluster must be idle
  • L2/L3 cache flushed (if not shared with other cluster)
  • Cluster interconnect powered down
  • Wake-up takes longer (more state to restore)
```

---

## 7. System Suspend (S2R / Suspend-to-RAM)

```
System-wide suspend (like laptop sleep):

  1. Linux PM: freeze all tasks, suspend all devices
  2. Linux calls PSCI SYSTEM_SUSPEND
  3. ATF:
     • Saves all secure state
     • Programs PMIC to enter standby
     • Configures wake-up sources (power button, RTC, network)
     • Cuts power to all cores, GPU, most peripherals
     
  4. System in sleep:
     • Only DRAM in self-refresh (retains contents)
     • Wake-up controller active (tiny power)
     • Total system power: ~10-50 mW (vs ~5W active)

  5. Wake-up event (button press):
     • PMIC powers on
     • Boot ROM BL1 starts again (cold reset)
     • BL1 → BL2 → BL31 detects warm boot
     • BL31 restores Secure state
     • Returns to Linux resume path
     • Linux resumes devices, thaws tasks
     • User sees: screen turns on, back where they left off

  Total wake-up time: ~100ms - 2s (depends on device count)
```

---

## 8. Power Domains

```
SoC power domains (independent power rails):

  ┌─────────────────────────────────────────────────────────┐
  │  Always-On Domain                                        │
  │  • Wake-up controller                                   │
  │  • RTC (Real-Time Clock)                                │
  │  • Interrupt controller (GIC wake-up logic)             │
  │  • PMIC interface                                        │
  ├─────────────────────────────────────────────────────────┤
  │  CPU Domain                                               │
  │  ┌────────────────┐  ┌────────────────┐                  │
  │  │ Core 0 domain  │  │ Core 1 domain  │  (per-core)     │
  │  │ • Core logic   │  │ • Core logic   │                  │
  │  │ • L1 cache     │  │ • L1 cache     │                  │
  │  └────────────────┘  └────────────────┘                  │
  │  ┌──────────────────────────────────┐                    │
  │  │ Cluster domain (shared)          │                    │
  │  │ • L2/L3 cache                    │                    │
  │  │ • Snoop control unit             │                    │
  │  └──────────────────────────────────┘                    │
  ├─────────────────────────────────────────────────────────┤
  │  Peripheral Domains                                       │
  │  • GPU domain (separate power rail)                      │
  │  • USB domain                                            │
  │  • Display domain                                        │
  │  • Modem domain (cellular)                               │
  └─────────────────────────────────────────────────────────┘
  
  Each domain can be powered on/off independently.
  Linux: drivers/soc/*/power-domains, genpd framework
```

---

Next: Back to [Power Management Overview](./README.md) | Continue to [SIMD & Floating Point →](../10_SIMD_FloatingPoint/)
