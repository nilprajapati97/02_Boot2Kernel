# `time_init()` — Architecture Time Source Setup

## Purpose

Sets up the architecture-specific clock event devices and clock sources for x86. This registers the TSC, HPET, and PIT as potential clocksources and programs the first clock event for the timer interrupt.

## Source File

`arch/x86/kernel/time.c`

```c
void __init time_init(void)
{
    late_time_init = x86_late_time_init;
}

static __init void x86_late_time_init(void)
{
    // Set up the HPET or PIT as the per-CPU clock event
    x86_init.timers.timer_init();    // → hpet_time_init() or setup_pit_timer()
    
    // Register TSC as clocksource (if stable)
    tsc_init();
}
```

## x86 Clocksources

### TSC (Time Stamp Counter)

The preferred clocksource on modern x86:

```asm
rdtsc                    ; Read TSC into EDX:EAX
; or:
rdtscp                   ; Read TSC + processor ID (serialized)
```

Properties:
- Available on all CPUs since Pentium
- Increments at CPU frequency (or constant rate with `constant_tsc`)
- `constant_tsc`: TSC runs at constant rate regardless of P-state (modern CPUs)
- `nonstop_tsc`: TSC doesn't stop during C-states
- `tsc_reliable`: TSC is synchronized across all CPUs (not always true)

### HPET (High Precision Event Timer)

- Memory-mapped counter running at ≥10MHz
- Can be shared across CPUs
- Used when TSC is not reliable
- Slower to read than RDTSC (MMIO vs register)

### PIT (8254 Programmable Interval Timer)

- Legacy ISA timer, 1.193182 MHz
- Used as fallback and for TSC calibration
- Programmed via I/O ports 0x40-0x43

## Clocksource Selection

The kernel selects the best available clocksource based on `rating`:

| Clocksource | Rating | Notes |
|-------------|--------|-------|
| `tsc-early` | 299 | Before TSC stability confirmed |
| `tsc` | 300 | Stable TSC (preferred) |
| `hpet` | 250 | HPET available |
| `acpi_pm` | 200 | ACPI power management timer |
| `jiffies` | 1 | Absolute fallback |

Runtime clocksource can be read/changed:
```bash
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
echo hpet > /sys/devices/system/clocksource/clocksource0/current_clocksource
```

## TSC Calibration

The TSC frequency is determined by:
1. CPUID leaf 0x15 (preferred — gives exact value)
2. CPUID leaf 0x16 (gives base frequency)
3. Calibration against HPET or PIT (timed loop)

```bash
dmesg | grep MHz
# tsc: Detected 3600.000 MHz processor
# tsc: Refined TSC clocksource calibration: 3600.019 MHz
```

## Pre-conditions

- ACPI tables available for HPET discovery
- `timekeeping_init()` complete (clocksource will be registered)

## Post-conditions

- TSC registered as clocksource
- HPET or PIT registered as clock event device
- First timer interrupt will fire after `local_irq_enable()`

## Cross-references

- [Phase overview](../README.md)
- `timekeeping_init()`: [../timekeeping_init/README.md](../timekeeping_init/README.md)
