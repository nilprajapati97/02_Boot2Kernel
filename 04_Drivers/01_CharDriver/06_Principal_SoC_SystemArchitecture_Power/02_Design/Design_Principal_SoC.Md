# Design: Principal/L6+ SoC System Architecture Driver
## Level 06 | End-to-End Deep Design from Scratch

---

## 1. Principal Engineer Mindset

At L6+ (Staff/Principal), the question is no longer "how to write a driver?" but:

- **How does this affect power budget?** (1 mA at 3.7V = 3.7 mW)
- **What is the worst-case latency?** (IRQ → userspace, scheduler jitter)
- **How does cache behavior affect throughput?** (false sharing, cold misses)
- **What happens at 40°C ambient with thermal throttling?**
- **Can this scale to 8 instances across NUMA nodes?**

---

## 2. SoC Resource Stack

A real Qualcomm/ARM SoC peripheral requires multiple resources:

```
┌─────────────────────────────────────────────────────────────┐
│                    Peripheral (e.g., UART, SPI, DSP)        │
├─────────────────────────────────────────────────────────────┤
│  Clock tree          │  Power domain    │  GPIO mux          │
│  core_clk (100MHz)   │  vdd (1.8V)      │  pinctrl (active)  │
│  bus_clk (AHB 50MHz) │  regulator       │  pinctrl (sleep)   │
├─────────────────────────────────────────────────────────────┤
│  IRQ (GIC SPI 42)    │  IOMMU          │  DMA engine        │
│  devm_request_irq    │  dma_map_sg     │  async transfers   │
├─────────────────────────────────────────────────────────────┤
│  MMIO registers (ioremap)                                   │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Clock Management

### Why Clocks Matter

Every SoC peripheral has:
- **Bus clock** — connects peripheral to AHB/AXI interconnect (access registers)
- **Core/functional clock** — runs peripheral logic (UART baud, SPI SCK source)

Without bus clock: register reads return garbage.
Without core clock: peripheral doesn't operate.

### Clock API

```c
/* Get clock handle from DT "clocks" property */
d->core_clk = devm_clk_get(&pdev->dev, "core");
d->bus_clk  = devm_clk_get(&pdev->dev, "bus");

/* Enable (two-step: prepare=slow, enable=fast) */
clk_prepare_enable(d->bus_clk);   /* must enable bus before core */
clk_set_rate(d->core_clk, 100000000);  /* set 100 MHz */
clk_prepare_enable(d->core_clk);

/* Disable */
clk_disable_unprepare(d->core_clk);
clk_disable_unprepare(d->bus_clk);  /* bus last */
```

### DT Clock Entry

```dts
peripheral@1000000 {
    clocks = <&clk_ctrl 5>, <&clk_ctrl 6>;
    clock-names = "core", "bus";
    clock-frequency = <100000000>;
};
```

---

## 4. Regulator (Power Supply) Management

### Why Regulators

SoC peripherals often share voltage rails or have separate sub-rails:
- `vdd-core` at 0.9V for logic
- `vdd-io` at 1.8V for I/O pads
- Enabling/disabling saves power (100–500 µA per domain)

```c
d->vdd = devm_regulator_get(&pdev->dev, "vdd");

/* Set voltage range acceptable to peripheral */
regulator_set_voltage(d->vdd, 1800000, 1800000);  /* 1.8V */
regulator_enable(d->vdd);

/* When done */
regulator_disable(d->vdd);
```

### DT Regulator Entry

```dts
peripheral@1000000 {
    vdd-supply = <&vreg_l6a_1p8>;  /* points to regulator node */
};
```

---

## 5. Pinctrl — GPIO Mux Configuration

SoC pins are multiplexed — same pin can be GPIO, UART TX, SPI MOSI, or I2C SDA.

```c
d->pinctrl       = devm_pinctrl_get(&pdev->dev);
d->pins_default  = pinctrl_lookup_state(d->pinctrl, "default");
d->pins_sleep    = pinctrl_lookup_state(d->pinctrl, "sleep");

/* Activate peripheral pinmux */
pinctrl_select_state(d->pinctrl, d->pins_default);

/* On suspend: switch to high-impedance / pull-down to save power */
pinctrl_select_state(d->pinctrl, d->pins_sleep);
```

### DT Pin Configuration

```dts
&pinctrl {
    peripheral_pins_default: peripheral-default {
        mux { pins = "gpio4", "gpio5"; function = "func1"; };
        config { pins = "gpio4"; drive-strength = <6>; bias-pull-up; };
    };
    peripheral_pins_sleep: peripheral-sleep {
        mux { pins = "gpio4", "gpio5"; function = "gpio"; };
        config { pins = "gpio4", "gpio5"; bias-pull-down; };
    };
};
```

---

## 6. Runtime Power Management (RPM)

### The Goal

Device wastes power if left on when idle. RPM auto-manages power:

```
open() ────────────────► pm_runtime_get_sync()  → rpm_resume() → power ON
         [idle N ms]
release() ─────────────► pm_runtime_put_autosuspend() → (after delay) rpm_suspend() → power OFF
```

### Implementation

```c
/* In probe: */
pm_runtime_enable(&pdev->dev);
pm_runtime_set_autosuspend_delay(&pdev->dev, 100);  /* 100ms */
pm_runtime_use_autosuspend(&pdev->dev);

/* Callbacks */
static int my_rpm_suspend(struct device *dev) {
    soc_power_off(priv);  /* clocks off, regulator off, pins sleep */
    return 0;
}
static int my_rpm_resume(struct device *dev) {
    soc_power_on(priv);   /* clocks on, regulator on, pins active */
    return 0;
}

static const struct dev_pm_ops my_pm = {
    SET_RUNTIME_PM_OPS(my_rpm_suspend, my_rpm_resume, NULL)
};
```

### Power Timeline

```
open()
  │ pm_runtime_get_sync → rpm_resume()
  │ clk_enable() + regulator_enable() + pinctrl_active()
  │
  │ [I/O active]
  │
release()
  │ pm_runtime_put_autosuspend()
  │ [100ms timer starts]
  │ rpm_suspend()
  │ clk_disable() + regulator_disable() + pinctrl_sleep()
```

---

## 7. Cache-Line Aware Structure Layout

False sharing: two unrelated fields in same 64-byte cache line — one CPU writing to field A causes CPU2's field B read to miss.

```c
/* BAD: counter and lock share cache line */
struct bad {
    atomic_t reads;    // 4 bytes
    atomic_t writes;   // 4 bytes
    spinlock_t lock;   // 4 bytes ← CPU0 writes this → invalidates reads on CPU1
};

/* GOOD: separate to different cache lines */
struct good {
    atomic_t reads;
    atomic_t writes;
} __cacheline_aligned;     /* pad to 64 bytes */

struct stats {
    spinlock_t lock;
} __cacheline_aligned;     /* own cache line */
```

---

## 8. Latency Budget — IRQ to Userspace

### Measurement

```c
/* In IRQ handler */
d->lat.last_irq = ktime_get();    /* ns-resolution timestamp */

/* In read() after copy_to_user */
d->lat.last_read = ktime_get();
latency_ns = ktime_to_ns(ktime_sub(lat.last_read, lat.last_irq));
```

### Typical Budget (Qualcomm 8-core SoC)

| Stage | Typical | Worst Case |
|-------|---------|-----------|
| HW → IRQ line | 50 ns | 200 ns |
| IRQ handler | 100 ns | 500 ns |
| wake_up → scheduler | 1–5 µs | 50 µs |
| Scheduler dispatch | 5–20 µs | 200 µs |
| copy_to_user | 100 ns | 1 µs |
| **Total IRQ→user** | **~10 µs** | **~500 µs** |

Scheduler jitter is the dominant source of latency variance.

---

## 9. Thermal Awareness

On Qualcomm devices, thermal throttling cuts clock rates:
- Core clock reduced from 2.8 GHz → 1.2 GHz
- Driver must handle reduced throughput gracefully

```c
/* Register thermal zone notification */
d->tz = thermal_zone_get_zone_by_name("cpu-thermal");

/* In periodic check */
int temp;
thermal_zone_get_temp(d->tz, &temp);
if (temp > 85000) {  /* > 85°C */
    clk_set_rate(d->core_clk, 50000000);  /* throttle to 50 MHz */
    dev_warn(&pdev->dev, "thermal throttle: temp=%d\n", temp);
}
```

---

## 10. Full System Architecture Diagram

```
User App (Android HAL)
     │ Binder IPC
     ▼
HAL (vendor.anil.sensor@1.0)
     │ /dev/MyAnilDev6
     ▼
VFS → principal_fops → principal_read()
     │ wait_event_interruptible (read_wq)
     │
     │ [SoC hardware generates interrupt]
     ▼
GIC → IRQ 42 → principal_irq()
  ├─ ktime timestamp
  ├─ push to kfifo
  └─ wake_up_interruptible(read_wq)
     │
     ▼
Scheduler dispatches reader task
     │ copy_to_user
     ▼
User App receives data
     │ pm_runtime_put_autosuspend()
     ▼
100ms idle → rpm_suspend()
  ├─ clk_disable
  ├─ regulator_disable
  └─ pinctrl_sleep → 0 power
```

---

## 11. Principal-Level Summary

| Topic | API | Impact |
|-------|-----|--------|
| Clock enable/disable | `clk_prepare_enable` | 5–50 mW saved |
| Regulator on/off | `regulator_enable` | 0.5–5 mW saved |
| Pin sleep config | `pinctrl_select_state` | <0.5 mW saved |
| Runtime PM | `pm_runtime_get/put` | Auto power gating |
| Cache alignment | `__cacheline_aligned` | Eliminates false sharing |
| Latency tracking | `ktime_get` | Measurable SLA |
| Thermal throttle | `thermal_zone_get_temp` | Prevents damage |
