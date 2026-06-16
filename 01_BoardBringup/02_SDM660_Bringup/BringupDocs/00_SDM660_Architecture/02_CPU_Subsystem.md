# CPU Subsystem — Kryo 260 (big.LITTLE)

## Overview

The SDM660 uses a **heterogeneous multi-processing (HMP)** architecture based on ARM's **big.LITTLE** concept. It combines high-performance "Gold" cores with power-efficient "Silver" cores, allowing the scheduler to place workloads on the most appropriate core type.

---

## CPU Topology

```
┌─────────────────────────────────────────────────────┐
│                CPU SUBSYSTEM                         │
│                                                     │
│  ┌──────────────────────┐  ┌──────────────────────┐│
│  │   GOLD CLUSTER        │  │   SILVER CLUSTER      ││
│  │   (Performance)       │  │   (Efficiency)        ││
│  │                      │  │                      ││
│  │  ┌──────┐ ┌──────┐  │  │  ┌──────┐ ┌──────┐  ││
│  │  │CPU 0 │ │CPU 1 │  │  │  │CPU 4 │ │CPU 5 │  ││
│  │  │Kryo  │ │Kryo  │  │  │  │Kryo  │ │Kryo  │  ││
│  │  │260   │ │260   │  │  │  │260   │ │260   │  ││
│  │  │Gold  │ │Gold  │  │  │  │Silver│ │Silver│  ││
│  │  └──┬───┘ └──┬───┘  │  │  └──┬───┘ └──┬───┘  ││
│  │  ┌──────┐ ┌──────┐  │  │  ┌──────┐ ┌──────┐  ││
│  │  │CPU 2 │ │CPU 3 │  │  │  │CPU 6 │ │CPU 7 │  ││
│  │  │Kryo  │ │Kryo  │  │  │  │Kryo  │ │Kryo  │  ││
│  │  │260   │ │260   │  │  │  │260   │ │260   │  ││
│  │  │Gold  │ │Gold  │  │  │  │Silver│ │Silver│  ││
│  │  └──┬───┘ └──┬───┘  │  │  └──┬───┘ └──┬───┘  ││
│  │     │        │       │  │     │        │       ││
│  │  ┌──▼────────▼──┐   │  │  ┌──▼────────▼──┐   ││
│  │  │  L2 Cache    │   │  │  │  L2 Cache    │   ││
│  │  │  1 MB        │   │  │  │  1 MB        │   ││
│  │  └──────────────┘   │  │  └──────────────┘   ││
│  └──────────────────────┘  └──────────────────────┘│
│                                                     │
│  ┌─────────────────────────────────────────────┐   │
│  │         APPS CLUSTER INTERCONNECT            │   │
│  │         (Cache Coherent Interconnect)         │   │
│  └──────────────────┬──────────────────────────┘   │
│                     │                               │
│  ┌──────────────────▼──────────────────────────┐   │
│  │              GIC-400                         │   │
│  │        Generic Interrupt Controller          │   │
│  └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

---

## Core Specifications

| Parameter | Gold Cluster | Silver Cluster |
|-----------|-------------|----------------|
| Core Type | Kryo 260 Gold | Kryo 260 Silver |
| ARM Base | Cortex-A73 derivative | Cortex-A53 derivative |
| Core Count | 4 | 4 |
| Max Frequency | 2.2 GHz | 1.8 GHz |
| L1 I-Cache | 64 KB per core | 32 KB per core |
| L1 D-Cache | 64 KB per core | 32 KB per core |
| L2 Cache | 1 MB shared | 1 MB shared |
| Pipeline | Out-of-order | In-order |
| ISA | ARMv8-A (AArch64) | ARMv8-A (AArch64) |
| VFP/NEON | Yes | Yes |

---

## How CPU Cores Boot

### Boot Sequence for CPU Cores

Only **CPU0 (Gold core 0)** boots initially. All other cores start in a powered-off state and are brought online by the kernel.

```
Power On
    │
    ▼
CPU0 (Gold Core 0) — Runs PBL → XBL → ABL → Kernel
    │
    ▼ (kernel boots to start_kernel)
    │
    ▼ (PSCI driver probes)
    │
    ▼ smp_init() called
    │
    ├── CPU1 brought online via PSCI CPU_ON
    ├── CPU2 brought online via PSCI CPU_ON
    ├── CPU3 brought online via PSCI CPU_ON
    ├── CPU4 brought online via PSCI CPU_ON
    ├── CPU5 brought online via PSCI CPU_ON
    ├── CPU6 brought online via PSCI CPU_ON
    └── CPU7 brought online via PSCI CPU_ON
```

### PSCI (Power State Coordination Interface)

PSCI is the ARM standard for CPU power management. The kernel uses PSCI to:
- **CPU_ON**: Bring a core out of reset
- **CPU_OFF**: Put a core into deepest sleep
- **CPU_SUSPEND**: Enter a low-power idle state
- **SYSTEM_RESET**: Reset the entire system
- **SYSTEM_OFF**: Power off the system

```
Device Tree Definition:
───────────────────────
psci {
    compatible = "arm,psci-1.0";
    method = "smc";        /* Use SMC (Secure Monitor Call) to invoke PSCI */
};

cpus {
    #address-cells = <2>;
    #size-cells = <0>;

    CPU0: cpu@0 {
        device_type = "cpu";
        compatible = "qcom,kryo260";
        reg = <0x0 0x0>;
        enable-method = "psci";
        capacity-dmips-mhz = <1024>;   /* Performance weight */
        next-level-cache = <&L2_0>;
    };

    CPU4: cpu@100 {
        device_type = "cpu";
        compatible = "qcom,kryo260";
        reg = <0x0 0x100>;
        enable-method = "psci";
        capacity-dmips-mhz = <670>;    /* Lower weight = efficiency core */
        next-level-cache = <&L2_1>;
    };
};
```

### CPU Power States (Idle)

The kernel's cpuidle driver manages these states for each core:

| State | Name | Description | Entry Latency | Exit Latency | Power |
|-------|------|-------------|---------------|--------------|-------|
| C0 | Active | Core running | 0 | 0 | Full |
| C1 | WFI | Wait-For-Interrupt | 1 μs | 1 μs | Low |
| C2 | Retention | Clock-gated, state retained | 20 μs | 20 μs | Very Low |
| C3 | PC (Power Collapse) | Core powered off, L2 retained | 100 μs | 250 μs | Minimal |
| C4 | Cluster PC | Entire cluster off | 500 μs | 1 ms | Near Zero |

---

## CPU Frequency Scaling (DVFS)

Each cluster has independent voltage and frequency scaling:

### Gold Cluster Frequency Table (typical)

| Frequency | Voltage (VDD_APC) |
|-----------|-------------------|
| 300 MHz | 0.568 V |
| 633 MHz | 0.644 V |
| 902 MHz | 0.708 V |
| 1113 MHz | 0.764 V |
| 1401 MHz | 0.848 V |
| 1536 MHz | 0.884 V |
| 1747 MHz | 0.940 V |
| 1843 MHz | 0.968 V |
| 2150 MHz | 1.032 V |
| 2208 MHz | 1.048 V |

### Kernel Interface

```bash
# View available frequencies
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies

# View current frequency
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# View governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# Set governor (interactive, performance, schedutil)
echo "schedutil" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

---

## CPU Topology in the Kernel

### Key Kernel Source Files

| File | Purpose |
|------|---------|
| `arch/arm64/kernel/head.S` | CPU boot entry point |
| `arch/arm64/kernel/smp.c` | SMP (multi-core) initialization |
| `arch/arm64/kernel/psci.c` | PSCI interface to TrustZone |
| `arch/arm64/kernel/cpuidle.c` | CPU idle states |
| `drivers/cpufreq/qcom-cpufreq-hw.c` | Qualcomm CPU frequency driver |
| `arch/arm64/boot/dts/qcom/sdm660.dtsi` | CPU node definitions |

### How the Kernel Discovers CPUs

1. **Device Tree**: Kernel reads `/cpus` node to find all CPU cores
2. **Topology**: `capacity-dmips-mhz` tells the scheduler each core's relative performance
3. **SMP Init**: `smp_init()` iterates through DT CPU nodes and calls `cpu_up()` for each
4. **PSCI Call**: `cpu_up()` → `psci_cpu_on()` → SMC call → TrustZone releases the core from reset
5. **Secondary Entry**: The new core starts executing at `secondary_entry` in `head.S`

```
Kernel Boot (CPU0 only)
    │
    ├── start_kernel()
    │       │
    │       ├── setup_arch()          → Parse DT /cpus node
    │       ├── init_IRQ()            → Initialize GIC-400
    │       └── rest_init()
    │               │
    │               └── kernel_init()
    │                       │
    │                       └── smp_init()
    │                               │
    │                               ├── cpu_up(1) → PSCI CPU_ON → CPU1 online
    │                               ├── cpu_up(2) → PSCI CPU_ON → CPU2 online
    │                               ├── ...
    │                               └── cpu_up(7) → PSCI CPU_ON → CPU7 online
    │
    └── All 8 cores now running Linux
```

---

## big.LITTLE Scheduling

The Linux **EAS (Energy Aware Scheduling)** framework or **HMP scheduler** decides which core runs each task:

- **Light tasks** (background, idle) → Silver cluster (power efficient)
- **Heavy tasks** (UI rendering, computation) → Gold cluster (high performance)
- **Task migration**: Tasks can move between clusters based on load

```
Task arrives
    │
    ▼
Scheduler checks task load
    │
    ├── Low load (< threshold) → Place on Silver core (save power)
    │
    └── High load (> threshold) → Place on Gold core (performance)
```

---

## Related Documents

- [../01_Power_On_Reset/](../01_Power_On_Reset/) — How CPU0 first powers on
- [../05_Linux_Kernel_Boot/01_Early_Assembly_Boot.md](../05_Linux_Kernel_Boot/01_Early_Assembly_Boot.md) — CPU0 kernel entry
- [../05_Linux_Kernel_Boot/07_CPU_Hotplug_PSCI.md](../05_Linux_Kernel_Boot/07_CPU_Hotplug_PSCI.md) — PSCI deep-dive
- [05_PMIC_PM660.md](05_PMIC_PM660.md) — VDD_APC voltage rail for CPU
