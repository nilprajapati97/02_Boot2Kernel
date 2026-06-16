# Phase 2: Architecture Setup

## Overview

This phase contains the most architecture-specific initialization in `start_kernel()`. It establishes the kernel's complete view of the hardware: physical memory layout, CPU features, per-CPU data structures, and the command line. After this phase, the kernel knows exactly how much RAM it has, which CPUs exist, and what parameters were passed at boot.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`early_security_init()`](early_security_init/README.md) | `security/security.c` | Initialize LSM framework ordering |
| 2 | [`setup_arch()`](setup_arch/README.md) | `arch/x86/kernel/setup.c` | **LARGEST call** — hardware detection, memory map |
| 3 | [`setup_boot_config()`](setup_boot_config/README.md) | `init/main.c` | Parse bootconfig from initrd |
| 4 | [`setup_command_line()`](setup_command_line/README.md) | `init/main.c` | Allocate cmdline copies |
| 5 | [`setup_nr_cpu_ids()`](setup_nr_cpu_ids/README.md) | `kernel/smp.c` | Compute nr_cpu_ids |
| 6 | [`setup_per_cpu_areas()`](setup_per_cpu_areas/README.md) | `mm/percpu.c` | Replicate per-CPU sections |
| 7 | [`smp_prepare_boot_cpu()`](smp_prepare_boot_cpu/README.md) | `arch/x86/kernel/smp.c` | Copy GDT/TSS to per-CPU area |
| 8 | [`boot_cpu_hotplug_init()`](boot_cpu_hotplug_init/README.md) | `kernel/cpu.c` | CPU hotplug state machine |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled

## Memory State

- **Entry**: `memblock` with basic static regions
- **Exit**: `memblock` fully populated with all RAM regions, reservations made

## Key Outputs of This Phase

After this phase, the kernel knows:
- How much RAM is available and its physical layout (`memblock`)
- Which CPU features are supported (CPUID flags)
- The kernel command line (`boot_command_line`, `saved_command_line`)
- How many CPUs are possible (`nr_cpu_ids`, `cpu_possible_mask`)
- Per-CPU data is set up for all possible CPUs

## The Importance of `setup_arch()`

`setup_arch()` is the most important function in this phase — and arguably in the entire boot sequence for hardware interaction. On x86-64 it is over 300 lines of direct code plus hundreds of lines in called functions. It bridges the gap between the hardware world (BIOS/UEFI tables, e820 memory map, CPUID) and the software world (memblock, cpufeatures, ACPI tables loaded into memory).

## Sub-directories

- [setup_arch/memory_map/](setup_arch/memory_map/README.md) — e820 memory map processing
- [setup_arch/cpu_detection/](setup_arch/cpu_detection/README.md) — CPUID and CPU feature flags
- [setup_arch/platform_devices/](setup_arch/platform_devices/README.md) — PCI early scan, ACPI tables
