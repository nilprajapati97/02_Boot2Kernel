# PSCI — CPU Bring-Up & Power Management

## Overview

**PSCI (Power State Coordination Interface)** is the ARM standard interface for CPU power management. On SDM660, Linux uses PSCI to bring up secondary CPUs, enter/exit CPU power states (idle, sleep), and perform system reset/shutdown. PSCI calls are handled by TrustZone firmware at EL3.

---

## PSCI Architecture

```
┌────────────────────────────────────────────────────────┐
│                PSCI Architecture                        │
│                                                        │
│  ┌─────────────────────────────────────────┐           │
│  │  Linux Kernel (EL1, Normal World)        │           │
│  │                                         │           │
│  │  PSCI API:                              │           │
│  │  ├── psci_cpu_on(cpu, entry_point)      │           │
│  │  ├── psci_cpu_off()                     │           │
│  │  ├── psci_cpu_suspend(power_state)      │           │
│  │  ├── psci_system_reset()                │           │
│  │  └── psci_system_off()                  │           │
│  │                                         │           │
│  │  Implementation: SMC instruction        │           │
│  └─────────────┬───────────────────────────┘           │
│                │ SMC call                               │
│  ┌─────────────▼───────────────────────────┐           │
│  │  TrustZone (EL3, Secure Monitor)        │           │
│  │                                         │           │
│  │  PSCI Handler:                          │           │
│  │  ├── Validates CPU ID                   │           │
│  │  ├── Controls CPU power controller      │           │
│  │  ├── Manages cache coherency            │           │
│  │  └── Coordinates with RPM               │           │
│  └─────────────┬───────────────────────────┘           │
│                │                                       │
│  ┌─────────────▼───────────────────────────┐           │
│  │  Hardware (CPU Power Controller)         │           │
│  │                                         │           │
│  │  ├── APCS power domain registers        │           │
│  │  ├── Per-core power gates               │           │
│  │  └── Cluster power gates                │           │
│  └─────────────────────────────────────────┘           │
└────────────────────────────────────────────────────────┘
```

---

## Secondary CPU Boot (SMP)

At boot, only CPU0 runs. The other 7 CPUs must be brought online:

```
CPU0 (running Linux)                          CPU1-7 (powered off)
────────────────────                          ─────────────────────
start_kernel()
    │
    ├── smp_init()
    │   └── cpu_up(1)  → bring CPU1 online
    │       │
    │       ├── psci_cpu_on(cpu=1, entry=secondary_entry)
    │       │   │
    │       │   └── SMC #0 → TrustZone
    │       │       │
    │       │       ├── TZ: Power on CPU1
    │       │       ├── TZ: Set CPU1 reset vector
    │       │       │       to secondary_entry
    │       │       └── TZ: Release CPU1 from reset
    │       │                                          │
    │       │                                     CPU1 wakes up
    │       │                                          │
    │       │                                     secondary_entry:
    │       │                                     ├── head.S (CPU init)
    │       │                                     ├── Enable MMU
    │       │                                     └── secondary_start_kernel()
    │       │                                          ├── Initialize GIC for this CPU
    │       │                                          ├── Enable local timer
    │       │                                          ├── Mark CPU online
    │       │                                          └── Enter scheduler (idle)
    │       │
    │       └── Wait for CPU1 to mark itself online
    │
    ├── cpu_up(2) → Same process for CPU2
    ├── ...
    └── cpu_up(7) → Same for CPU7

    All 8 CPUs now online and running
```

---

## CPU Power States (LPM)

```
┌──────────┬───────────────────┬───────────────┬──────────────┬──────────────┐
│  State   │  Description       │  Exit Latency │  Power       │  What's Lost │
├──────────┼───────────────────┼───────────────┼──────────────┼──────────────┤
│  C0      │  Active (running)  │  0            │  Full        │  Nothing     │
│  C1/WFI  │  Clock gated       │  ~1 μs        │  ~70% of C0  │  Nothing     │
│  C2      │  Retention         │  ~100 μs      │  ~20% of C0  │  Nothing*    │
│  C3      │  Power Collapse    │  ~500 μs      │  ~5% of C0   │  CPU state   │
│  C4      │  Cluster Collapse  │  ~2 ms        │  Near zero   │  L2 cache    │
└──────────┴───────────────────┴───────────────┴──────────────┴──────────────┘

* Retention mode: registers preserved via retention voltage
```

### CPU Idle Flow (C3 — Power Collapse)

```
CPU becomes idle (no runnable tasks)
    │
    ├── cpuidle governor selects C3 (power collapse)
    │
    ├── CPU saves context:
    │   ├── General purpose registers (x0-x30)
    │   ├── System registers (SCTLR, TTBR, VBAR, etc.)
    │   ├── GIC CPU interface state
    │   └── Save to reserved memory area
    │
    ├── psci_cpu_suspend(C3_power_state)
    │   └── SMC → TrustZone
    │       ├── TZ saves secure state
    │       ├── TZ tells RPM: "CPU N going to power collapse"
    │       ├── TZ power-gates the CPU core
    │       └── CPU is now OFF (no power, no clocks)
    │
    │   ... time passes ...
    │
    ├── Wake-up interrupt arrives (timer, IPI, peripheral)
    │   ├── RPM detects interrupt for powered-off CPU
    │   ├── RPM restores CPU power
    │   ├── TZ restores secure state
    │   └── TZ jumps to kernel's cpu_resume entry
    │
    └── CPU restores context:
        ├── Restore registers from saved area
        ├── Restore GIC state
        └── Return to idle loop → pick up next task
```

---

## PSCI in Device Tree

```dts
psci {
    compatible = "arm,psci-1.0";
    method = "smc";  /* Use SMC instruction to call PSCI */
};

cpus {
    CPU0: cpu@0 {
        enable-method = "psci";  /* Use PSCI to bring up this CPU */
    };
    CPU4: cpu@100 {
        enable-method = "psci";
    };
};
```

---

## PSCI Function IDs

| Function | ID (SMC64) | Description |
|----------|-----------|-------------|
| PSCI_VERSION | 0x84000000 | Query PSCI version |
| CPU_SUSPEND | 0xC4000001 | Enter low-power state |
| CPU_OFF | 0x84000002 | Power off calling CPU |
| CPU_ON | 0xC4000003 | Power on target CPU |
| SYSTEM_RESET | 0x84000009 | System reset |
| SYSTEM_OFF | 0x84000008 | System shutdown |
| CPU_FREEZE | 0x84000003 | Deepest idle state |

---

## Kernel Implementation

```c
/* drivers/firmware/psci.c */

static int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
    return invoke_psci_fn(PSCI_FN_CPU_ON, cpuid, entry_point, 0);
}

/* invoke_psci_fn uses SMC: */
static unsigned long __invoke_psci_fn_smc(unsigned long function_id,
                    unsigned long arg0, unsigned long arg1,
                    unsigned long arg2)
{
    struct arm_smccc_res res;
    arm_smccc_smc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);
    return res.a0;
}
```

---

## Debugging CPU States

```bash
# CPU online status
adb shell cat /sys/devices/system/cpu/online
# 0-7

# CPU idle state info
adb shell cat /sys/devices/system/cpu/cpu0/cpuidle/state3/name
# pc (power collapse)

adb shell cat /sys/devices/system/cpu/cpu0/cpuidle/state3/latency
# 500 (microseconds)

adb shell cat /sys/devices/system/cpu/cpu0/cpuidle/state3/usage
# 12345 (times entered)

# PSCI version
adb shell cat /sys/firmware/devicetree/base/psci/compatible
# arm,psci-1.0

# Disable a CPU (hotplug)
adb shell echo 0 > /sys/devices/system/cpu/cpu7/online
```

---

## Related Documents

- [01_Early_Assembly_Boot.md](01_Early_Assembly_Boot.md) — CPU entry (head.S)
- [05_GIC_Interrupt_Controller.md](05_GIC_Interrupt_Controller.md) — IPI for CPU wakeup
- [../02_XBL_Secondary_Bootloader/05_TrustZone_QSEE_Init.md](../02_XBL_Secondary_Bootloader/05_TrustZone_QSEE_Init.md) — TZ handles PSCI
- [../03_RPM_Firmware/01_RPM_Overview.md](../03_RPM_Firmware/01_RPM_Overview.md) — RPM coordinates CPU sleep
