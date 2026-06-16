# Phase 14: ACPI, Clocking, and CPU Finalization

## Overview

Initializes ACPI (Advanced Configuration and Power Interface), stabilizes the scheduler clock source, calibrates the delay loop (BogoMIPS), and finalizes CPU feature detection.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`acpi_early_init()`](acpi_early_init/README.md) | `drivers/acpi/bus.c` | ACPI namespace and AML interpreter |
| 2 | [`acpi_subsystem_init()`](acpi_subsystem_init/README.md) | `drivers/acpi/bus.c` | Full ACPI bus initialization |
| 3 | [`sched_clock_init()`](sched_clock_init/README.md) | `kernel/sched/clock.c` | Stable scheduler clock |
| 4 | [`calibrate_delay()`](calibrate_delay/README.md) | `init/calibrate.c` | BogoMIPS measurement |
| 5 | [`arch_cpu_finalize_init()`](arch_cpu_finalize_init/README.md) | `arch/x86/kernel/cpu/common.c` | CPU feature finalization |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## Function Index

- [acpi_early_init/](acpi_early_init/README.md)
- [acpi_subsystem_init/](acpi_subsystem_init/README.md)
- [sched_clock_init/](sched_clock_init/README.md)
- [calibrate_delay/](calibrate_delay/README.md)
- [arch_cpu_finalize_init/](arch_cpu_finalize_init/README.md)
