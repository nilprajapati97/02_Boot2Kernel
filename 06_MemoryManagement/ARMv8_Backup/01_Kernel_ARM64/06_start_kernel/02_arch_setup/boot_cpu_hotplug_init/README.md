# `boot_cpu_hotplug_init()` — CPU Hotplug State Machine

## Purpose

Initializes the CPU hotplug infrastructure for the boot CPU by setting its hotplug state to `CPUHP_ONLINE` and initializing the per-CPU `cpuhp_state` structure. This allows the CPU hotplug framework to track the boot CPU's lifecycle alongside secondary CPUs.

## Source File

`kernel/cpu.c`

## CPU Hotplug Overview

Linux supports adding and removing CPUs at runtime (`CONFIG_HOTPLUG_CPU`). Each CPU has a state machine with states ranging from `CPUHP_OFFLINE` to `CPUHP_ONLINE`. The transitions between states call registered callbacks.

```
CPUHP_OFFLINE
    ↓ (cpu_up)
CPUHP_BRINGUP_CPU
    ↓
CPUHP_AP_ONLINE
    ↓
...intermediate states...
    ↓
CPUHP_ONLINE
    ↑↓ (runtime hotplug up/down)
CPUHP_ONLINE
```

## What `boot_cpu_hotplug_init()` Does

1. Sets `per_cpu(cpuhp_state, smp_processor_id()).state = CPUHP_ONLINE` — the boot CPU is already online
2. Sets `per_cpu(cpuhp_state, smp_processor_id()).target = CPUHP_ONLINE`
3. Initializes any per-CPU hotplug data structures

## Pre-conditions

- Per-CPU areas must be set up (`setup_per_cpu_areas()`)
- Boot CPU must be identified (`smp_setup_processor_id()`)

## Post-conditions

- Boot CPU's hotplug state is correctly set to `CPUHP_ONLINE`
- Hotplug callbacks can safely query the boot CPU's state

## IRQ State

IRQs **disabled** — per-CPU write.

## Kconfig Dependencies

- `CONFIG_HOTPLUG_CPU`: Required for full hotplug support; without it, a simpler stub is used

## Cross-references

- [Phase overview](../README.md)
- `smp_init()` — brings secondary CPUs online: [../../19_rest_init/kernel_init/kernel_init_freeable/smp_init/README.md](../../19_rest_init/kernel_init/kernel_init_freeable/smp_init/README.md)
