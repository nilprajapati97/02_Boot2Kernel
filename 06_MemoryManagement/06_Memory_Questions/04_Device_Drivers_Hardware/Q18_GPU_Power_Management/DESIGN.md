# Q18 — Design a Power Management System for GPU Devices

---

## 1. Problem Statement

GPU power management must balance performance and power consumption across a wide range of workloads — from idle (milliwatts) to full compute (400W for datacenter GPUs). The system must:
- Dynamically scale GPU frequency and voltage (DVFS — Dynamic Voltage and Frequency Scaling).
- Gate power to idle GPU engines (clock gating, power gating).
- Integrate with Linux runtime PM to allow system-level sleep states.
- Handle thermal management: throttle frequency when temperature exceeds threshold.
- Maintain firmware-negotiated power contracts with PCIe power management.

Design the complete GPU PM stack from kernel infrastructure to hardware control.

---

## 2. Requirements

### 2.1 Functional Requirements
- DVFS: frequency selection based on GPU load (utilization feedback loop).
- Clock gating per GPU engine (idle engines gated immediately).
- Power gating: power off entire GPU when idle for > T seconds.
- Thermal throttling: reduce frequency when GPU temp > threshold.
- PCIe D-states: D0 (active), D3hot (suspended, config space accessible).
- Runtime PM integration: GPU auto-suspended after inactivity timeout.

### 2.2 Non-Functional Requirements
- Frequency ramp latency: < 1 ms from load detection to new frequency.
- Power gating transition latency: < 50 ms (acceptable for user-perceptible apps).
- Temperature polling interval: ≤ 100 ms (thermal hazard response time).
- Idle power: < 5W in D3hot; < 1W in D3cold (if supported).

---

## 3. Constraints & Assumptions

- Linux `devfreq` subsystem for DVFS.
- Linux `runtime PM` (`CONFIG_PM`) for device sleep state management.
- Linux `hwmon` subsystem for temperature monitoring.
- PCIe D-state transitions via Linux PCI PM.
- Hardware: GPU with SMU (System Management Unit) firmware co-processor managing VID/FID tables.

---

## 4. Architecture Overview

```
  GPU Load Monitor                 Thermal Monitor
  (GPU utilization %)              (NV/AMD temp sensor)
         │                                │
         ▼                                ▼
  ┌──────────────────────┐    ┌────────────────────────┐
  │  devfreq governor    │    │  hwmon + thermal_zone  │
  │  (simple_ondemand)   │    │  trip points: passive  │
  │  target_freq = f(u%) │    │  critical 95°C          │
  └──────────┬───────────┘    └────────────┬───────────┘
             │                             │ throttle
             ▼                             ▼
  ┌──────────────────────────────────────────────────┐
  │  GPU PM Core (kernel driver)                     │
  │  ┌──────────────────┐  ┌────────────────────┐   │
  │  │  DVFS Controller │  │  Power State FSM   │   │
  │  │  freq/voltage    │  │  D0 ↔ D3hot ↔      │   │
  │  │  table (OPP)     │  │  D3cold            │   │
  │  └──────────────────┘  └────────────────────┘   │
  │  ┌──────────────────┐  ┌────────────────────┐   │
  │  │  Clock Gate Ctrl │  │  Runtime PM        │   │
  │  │  per-engine      │  │  autosuspend timer │   │
  └──┼──────────────────┼──┼────────────────────┼───┘
     │                  │  │                    │
     ▼                  ▼  ▼                    ▼
  SMU Firmware       GPU Engine             PCIe D-state
  (VID/FID tables)   Clock Gates            (ASPM / PME)
```

---

## 5. Core Data Structures

### 5.1 OPP (Operating Performance Point) Table

```c
/* Each OPP: one valid (freq, voltage) combination */
/* Populated from device tree or ACPI/_DSM table */

/* Linux OPP table (managed by dev_pm_opp_*) */
struct dev_pm_opp {
    struct list_head  node;
    unsigned long     rate;       /* Hz */
    unsigned long     u_volt;     /* µV */
    unsigned long     u_volt_min; /* voltage margin */
    unsigned long     u_volt_max;
    bool              available;
    bool              turbo;      /* boost state */
};

/* GPU-specific OPP + power state */
struct my_gpu_opp_entry {
    u32  freq_mhz;    /* GPU core frequency */
    u32  mem_mhz;     /* VRAM frequency */
    u32  volt_mv;     /* core voltage */
    u32  power_mw;    /* typical TDP at this OPP */
    u8   pstate;      /* SMU performance state index */
};
```

### 5.2 devfreq Device

```c
struct devfreq {
    struct device       *dev;
    struct devfreq_dev_profile *profile;  /* get_dev_status, target callbacks */
    const char          *governor_name;   /* "simple_ondemand", "performance" */
    struct devfreq_governor *governor;
    unsigned long        min_freq;
    unsigned long        max_freq;
    unsigned long        previous_freq;   /* last set frequency */
    struct mutex         lock;
    /* Stats */
    struct devfreq_stats stats;
};

struct devfreq_dev_status {
    unsigned long  total_time;    /* total time in current period (ns) */
    unsigned long  busy_time;     /* GPU busy time (ns) */
    unsigned long  current_frequency;
    void          *private_data;
};
```

### 5.3 Thermal Zone

```c
struct thermal_zone_device {
    char                        type[THERMAL_NAME_LENGTH];
    struct device               device;
    struct thermal_zone_params *tzp;   /* governor params */
    int                         temperature;    /* current °C × 1000 */
    int                         last_temperature;
    /* Trip points */
    int                         num_trips;
    struct thermal_trip         trips[];
    /* Cooling devices bound to this zone */
    struct list_head            thermal_instances;
};

struct thermal_trip {
    int              temperature;     /* threshold °C × 1000 */
    int              hysteresis;      /* hysteresis °C × 1000 */
    enum thermal_trip_type type;      /* THERMAL_TRIP_PASSIVE, CRITICAL */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 DVFS — Frequency Selection Algorithm (simple_ondemand)

```c
/* Devfreq simple_ondemand governor */
static int devfreq_simple_ondemand_func(struct devfreq *df,
                                         unsigned long *freq)
{
    struct devfreq_dev_status *stat = &df->last_status;
    unsigned long long a, b;

    /* Utilization = busy_time / total_time */
    a = stat->busy_time * 100;
    b = div64_u64(a, stat->total_time);   /* b = utilization % */

    if (b >= UPTHRESHOLD) {
        /* High load: request maximum frequency */
        *freq = df->max_freq;
    } else if (b < DOWNDIFFERENTIAL) {
        /* Low load: scale down proportionally */
        *freq = stat->current_frequency * b / UPTHRESHOLD;
        *freq = max(*freq, df->min_freq);
    } else {
        /* Mid range: maintain current */
        *freq = stat->current_frequency;
    }
    return 0;
}
```

### 6.2 Voltage-Frequency Scaling (SMU Firmware Interface)

```
DVFS sequence (safe: always change voltage before frequency):

Frequency INCREASE:
    1. Request higher voltage from SMU: smu_send_msg(SMU_MSG_SetVoltage, new_volt)
    2. Wait for voltage settled: poll SMU status register (< 1ms)
    3. Write new PLL divider: reg_write(GPU_PLL_CTRL, new_divider)
    4. Wait for PLL lock: poll PLL_STATUS register

Frequency DECREASE (reverse order — safer if voltage drops too early):
    1. Write new PLL divider (lower freq)
    2. Wait for PLL lock
    3. Request lower voltage from SMU
```

### 6.3 Clock Gating — Per-Engine

```c
/* Each GPU engine has a clock gate enable bit in control register */
#define ENG_CLK_GATE_CTRL    0x1234  /* BAR0 offset */
#define ENG_CLK_GATE_COMPUTE BIT(0)
#define ENG_CLK_GATE_COPY    BIT(1)
#define ENG_CLK_GATE_VID     BIT(2)

static void my_gpu_engine_clock_gate(struct my_gpu_dev *dev,
                                      enum gpu_engine eng, bool gate)
{
    u32 val = my_reg_read32(dev, ENG_CLK_GATE_CTRL);
    if (gate)
        val |= BIT(eng);    /* gate: stop clock to engine */
    else
        val &= ~BIT(eng);   /* ungate: resume clock */
    my_reg_write32(dev, ENG_CLK_GATE_CTRL, val);
}

/* Called from engine submit/complete hooks: */
/* On last job complete → gate engine clock */
/* On new job submit → ungate engine clock */
```

### 6.4 Runtime PM — Auto-Suspend

```c
/* Driver init: configure runtime PM autosuspend */
pm_runtime_set_autosuspend_delay(&pdev->dev, 2000); /* 2 second idle timeout */
pm_runtime_use_autosuspend(&pdev->dev);
pm_runtime_enable(&pdev->dev);

/* Before accessing device registers: */
pm_runtime_get_sync(&pdev->dev);  /* wake device if suspended */

/* After done with device: */
pm_runtime_put_autosuspend(&pdev->dev);  /* start idle timer */

/* Runtime suspend callback: */
static int my_gpu_runtime_suspend(struct device *dev)
{
    my_gpu_save_state(gpu_dev);       /* save register state */
    my_gpu_power_gate(gpu_dev);       /* gate all power domains */
    pci_save_state(pdev);             /* save PCIe config space */
    pci_set_power_state(pdev, PCI_D3hot); /* PCIe D3hot */
    return 0;
}

/* Runtime resume callback: */
static int my_gpu_runtime_resume(struct device *dev)
{
    pci_set_power_state(pdev, PCI_D0);
    pci_restore_state(pdev);          /* restore PCIe config space */
    my_gpu_power_ungate(gpu_dev);
    my_gpu_restore_state(gpu_dev);    /* restore registers */
    return 0;
}
```

### 6.5 Thermal Throttling via Cooling Device

```c
/* Register GPU as a cooling device */
static struct thermal_cooling_device_ops gpu_cooling_ops = {
    .get_max_state  = gpu_cooling_get_max_state,    /* return max throttle level */
    .get_cur_state  = gpu_cooling_get_cur_state,
    .set_cur_state  = gpu_cooling_set_cur_state,    /* apply throttle */
};

int gpu_cooling_set_cur_state(struct thermal_cooling_device *cdev,
                               unsigned long state)
{
    /* state 0 = no throttle, state N = max throttle (minimum freq) */
    unsigned long target_freq = max_freq - (state * freq_step);
    target_freq = max(target_freq, min_freq);

    /* Update devfreq max frequency constraint */
    dev_pm_qos_update_request(&gpu_dev->thermal_freq_req,
                               target_freq / 1000);  /* kHz */
    return 0;
}
```

---

## 7. Trade-off Analysis

| Decision | Aggressive PM | Conservative PM | Reason |
|---|---|---|---|
| Autosuspend delay | 0.5s | 10s | Short = more power saving; long = less wake-up latency |
| DVFS threshold | Upthreshold=80% | Upthreshold=95% | Low threshold = more freq bumps; high = more time at lower freq |
| Thermal throttle granularity | 10 MHz steps | 100 MHz steps | Fine = smoother thermal management; coarse = simpler logic |
| Clock gating latency | Immediate | After 10 ms idle | Immediate saves power; delayed avoids thrashing for bursty engines |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| devfreq core | `drivers/devfreq/devfreq.c` | `devfreq_add_device()`, `update_devfreq()` |
| simple_ondemand governor | `drivers/devfreq/governor_simpleondemand.c` | `devfreq_simple_ondemand_func()` |
| OPP library | `drivers/opp/core.c` | `dev_pm_opp_add()`, `dev_pm_opp_find_freq_ceil()` |
| Runtime PM | `drivers/base/power/runtime.c` | `pm_runtime_get_sync()`, `pm_runtime_put_autosuspend()` |
| PCIe PM | `drivers/pci/pci.c` | `pci_set_power_state()`, `pci_save_state()` |
| Thermal zone | `drivers/thermal/thermal_core.c` | `thermal_zone_device_register()` |
| Cooling device | `drivers/thermal/thermal_core.c` | `thermal_cooling_device_register()` |
| AMD GPU PM | `drivers/gpu/drm/amd/pm/` | `amdgpu_dpm_enable()`, `amdgpu_devfreq_target()` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 GPU Stuck in D3hot After Suspend
```bash
cat /sys/bus/pci/devices/0000:03:00.0/power_state  # D0 or D3hot?
cat /sys/bus/pci/devices/0000:03:00.0/runtime_status  # active/suspended
# Force wake: echo on > /sys/bus/pci/devices/0000:03:00.0/power/control
```

### 9.2 Thermal Shutdown
```bash
dmesg | grep "thermal\|overheat\|throttl"
cat /sys/class/thermal/thermal_zone*/temp   # read temps (°C × 1000)
cat /sys/class/hwmon/hwmon*/temp*_input      # hwmon readings
# Verify cooling device state: cat /sys/class/thermal/cooling_device*/cur_state
```

### 9.3 DVFS Stuck at Low Frequency
```bash
cat /sys/class/devfreq/*/cur_freq   # current frequency
cat /sys/class/devfreq/*/available_frequencies
cat /sys/class/devfreq/*/governor   # current governor
# Thermal constraint check:
cat /sys/bus/pci/devices/0000:03:00.0/power/wakeup
```

---

## 10. Performance Considerations

- **Wake-up latency:** D3hot → D0 takes 10–50 ms. For interactive applications, keep GPU in D0 with clock/power gating only (µs-level wake-up).
- **Voltage regulator slew rate:** Physical VRM (voltage regulator module) limits how fast voltage can change (~50 mV/µs). Frequency ramp is bounded by this.
- **PCIe ASPM (Active State Power Management):** PCIe link power states (L0s, L1, L1.1, L1.2). L1.2 saves most power but has ~100 µs re-entry latency — disable for low-latency devices.
- **SMU mailbox polling:** Avoid polling SMU status in interrupt context; use a dedicated PM workqueue.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. devfreq + OPP: DVFS is not custom — use standard Linux infrastructure.
2. Voltage-before-frequency on ramp-up; frequency-before-voltage on ramp-down — stability critical.
3. Runtime PM `pm_runtime_get_sync()` / `pm_runtime_put_autosuspend()` — wrap all register accesses.
4. Thermal framework: thermal_zone + cooling_device + trip_point → decouple temperature sensing from frequency control.
5. Clock gating vs power gating: clock gating is µs-latency, power gating is ms-latency.
6. PCIe D3hot vs D3cold: D3cold removes power entirely, requires OS ACPI support to re-enable.
7. SMU co-processor: hardware policy engine separate from GPU compute — know this exists in production chips.
