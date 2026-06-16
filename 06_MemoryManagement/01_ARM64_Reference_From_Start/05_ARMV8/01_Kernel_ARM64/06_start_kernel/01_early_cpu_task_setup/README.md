# Phase 1: Early CPU & Task Setup

## Overview

This phase covers the very first actions performed by `start_kernel()` before interrupts are explicitly disabled. These calls establish the bare minimum safety infrastructure: a stack guard for the boot task, CPU identity, object lifecycle debugging, and the root of the cgroup hierarchy.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`set_task_stack_end_magic()`](set_task_stack_end_magic/README.md) | `kernel/fork.c` | Stack overflow sentinel |
| 2 | [`smp_setup_processor_id()`](smp_setup_processor_id/README.md) | `kernel/smp.c` | Boot CPU identification |
| 3 | [`debug_objects_early_init()`](debug_objects_early_init/README.md) | `lib/debugobjects.c` | Object lifecycle tracking |
| 4 | [`init_vmlinux_build_id()`](init_vmlinux_build_id/README.md) | `kernel/buildid.c` | ELF build-ID recording |
| 5 | [`cgroup_init_early()`](cgroup_init_early/README.md) | `kernel/cgroup/cgroup.c` | Root cgroup pre-allocation |
| 6 | `local_irq_disable()` | inline | **IRQs explicitly disabled** |
| 7 | [`boot_cpu_init()`](boot_cpu_init/README.md) | `kernel/cpu.c` | CPU bitmask initialization |
| 8 | [`page_address_init()`](page_address_init/README.md) | `mm/highmem.c` | Highmem page address table |

## IRQ State

- **Entry**: IRQs may be on or off depending on arch (typically off from assembly)
- **Exit**: IRQs **disabled** (explicitly via `local_irq_disable()` at step 6)

## Memory State

- **Entry**: `memblock` static regions only
- **Exit**: Same — no allocations occur in this phase

## Pre-conditions (at entry to phase)

- Architecture assembly has completed (paging on, stack set up)
- `init_task` is statically allocated and accessible
- `__setup_start` / `__setup_end` linker symbols are valid

## Post-conditions (after phase)

- Stack overflow protection is armed for the boot CPU
- Boot CPU ID is recorded
- Root cgroup is allocated
- Boot CPU is visible in `cpu_online_mask`
- IRQs are disabled and will remain so until line ~993

## Dependencies on Prior Phases

None — this is the first phase.

## Key Global Variables Modified

| Variable | Set by | Value After |
|----------|--------|-------------|
| `early_boot_irqs_disabled` | `local_irq_disable()` wrapper | `true` |
| `init_task.stack_canary` | `set_task_stack_end_magic()` | `STACK_END_MAGIC` |
| `cpu_online_mask` | `boot_cpu_init()` | bit 0 set |
| `cpu_possible_mask` | `boot_cpu_init()` | bit 0 set |
