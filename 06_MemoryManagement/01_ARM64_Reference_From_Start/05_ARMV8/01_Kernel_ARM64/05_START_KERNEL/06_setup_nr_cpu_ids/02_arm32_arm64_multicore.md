# 02. ARM32, ARM64 and Multicore Behavior for setup_nr_cpu_ids

## Purpose

This document explains how the Linux kernel reaches `setup_nr_cpu_ids()` on ARM32 and ARM64, how the two architectures differ in early CPU discovery, and how multicore CPU semantics are represented in Linux.

## Generic `setup_nr_cpu_ids()` path

The generic function is defined in `linux/kernel/smp.c`:

```c
void __init setup_nr_cpu_ids(void)
{
    set_nr_cpu_ids(find_last_bit(cpumask_bits(cpu_possible_mask), NR_CPUS) + 1);
}
```

That means both ARM32 and ARM64 use the same generic routine once `cpu_possible_mask` is available.

## What `cpu_possible_mask` means

`cpu_possible_mask` is the bitmap of all CPU IDs that the kernel considers possible for this boot. It is platform-specific and must be initialized before `setup_nr_cpu_ids()` runs.

Linux uses three CPU masks:

- `cpu_possible_mask`: possible CPU IDs for this boot
- `cpu_present_mask`: CPUs physically present now
- `cpu_online_mask`: CPUs already online and scheduling work

`setup_nr_cpu_ids()` uses only `cpu_possible_mask`.

## ARM64 behavior

### `setup_arch()` on ARM64

In `arch/arm64/kernel/setup.c`, the ARM64 `setup_arch()` implementation does:

```c
*cmdline_p = boot_command_line;
```

That means ARM64 typically uses `boot_command_line` directly as the parse buffer. It does not make a separate copy before `setup_command_line()`.

### CPU enumeration on ARM64

ARM64 discovers CPUs through one of two sources:

- Device Tree (`/cpus` nodes) when ACPI is disabled
- ACPI MADT entries when ACPI is enabled

The CPU enumeration path does these things:

1. build `cpu_logical_map[]` from hardware IDs
2. validate boot CPU is logical ID 0
3. parse the DT or MADT to discover possible CPUs
4. map logical CPUs to NUMA nodes early

### `smp_prepare_cpus()` on ARM64

ARM64 calls `smp_prepare_cpus(setup_max_cpus)` after `setup_nr_cpu_ids()` and `setup_per_cpu_areas()`. The platform-specific CPU prepare code initializes topology and, if there are multiple CPUs, enables the secondary processors.

## ARM32 behavior

### `setup_arch()` on ARM32

In `arch/arm/kernel/setup.c`, ARM32 does additional work:

```c
strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
*cmdline_p = cmd_line;
```

This means ARM32 preserves the raw architecture boot string in `boot_command_line` and supplies a writable copy in `cmd_line[]` for parsing.

### Why ARM32 makes a copy

Linux parser functions mutate the command-line string in place. ARM32 keeps `boot_command_line` unchanged so other code can still refer to the untouched bootloader command string, while parsing operates on the writable copy.

### CPU enumeration on ARM32

ARM32 CPU discovery is typically done via:

- ATAGs boot tags
- Device Tree
- early machine descriptions and platform code

The ARM32 `setup_arch()` path may populate `cpu_possible_mask` from the DT or from platform-specific fixed core counts.

## Comparison

### Common facts

- After architecture-specific initialization, both ARM32 and ARM64 call the same `setup_nr_cpu_ids()`.
- `setup_nr_cpu_ids()` is architecture-neutral.
- Both architectures must provide a correct `cpu_possible_mask`.
- Both architectures may support `maxcpus=` and `nr_cpus=`.

### ARM32 differences

- uses a separate `cmd_line` parse buffer
- may rely more heavily on legacy ATAGs or legacy machine descriptions
- platform code often explicitly sets `cpu_possible_mask` and may call `init_cpu_present()` earlier

### ARM64 differences

- uses `boot_command_line` directly
- CPU discovery is strongly tied to DT and ACPI MADT
- builds `cpu_logical_map` from `MPIDR` values

## Multicore and Linux CPU semantics

Linux abstracts CPU hardware into three related concepts:

### 1. `possible`

The set of CPU IDs that could exist. This is a static boot-time view and is represented by `cpu_possible_mask`.

### 2. `present`

The set of CPU IDs actually available right now. On non-hotplug systems, this usually equals `possible`. On hotplug-enabled systems, it can be smaller.

### 3. `online`

The CPUs that are currently enabled and running scheduler tasks.

### Why this matters

When Linux boots on SMP hardware, it often knows the maximum possible CPU ID before all CPUs are online. `setup_nr_cpu_ids()` lets the kernel size its CPU data structures early, even if secondary CPUs are not yet brought online.

## `maxcpus=` and `nr_cpus=` semantics

### `maxcpus=`

- limits how many CPUs will actually be activated
- stored in `setup_max_cpus`
- affects later SMP bring-up, not `nr_cpu_ids`

### `nr_cpus=`

- limits the total number of CPU IDs visible to the kernel
- may reduce `nr_cpu_ids` if lower than the platform maximum
- used for limiting the ID space itself, including possible and present CPU IDs

## Multi-OS concepts in Linux CPU setup

Linux’s CPU-ID model is designed for portability across many platforms and boot environments:

- CPU IDs are logical abstractions, not hardware core IDs
- boot CPU is always logical CPU 0
- CPU IDs can be clipped by firmware, hypervisor, or boot parameters
- the same generic structures support UP, SMP, hotplug, and virtualized guests

### Why logical CPU IDs matter

The kernel does not assume a strict one-to-one relationship between logical CPU numbers and physical core IDs. Instead, it uses logical IDs to index per-CPU data and CPU masks.

This allows Linux to run on:

- homogeneous multicore systems
- heterogeneous clusters with big.LITTLE and SMT
- virtualized guests where the hypervisor may present a reduced CPU set
- systems that can hotplug or power off cores at runtime

## Summary

- `setup_nr_cpu_ids()` is generic and common to ARM32 and ARM64.
- The ARM-specific difference is the architecture’s earlier command line and CPU enumeration work.
- ARM32 preserves the raw command line in `boot_command_line` and parses a copy.
- ARM64 typically uses `boot_command_line` directly.
- Multicore Linux uses `possible`, `present`, and `online` masks so CPU limits can be set early while still supporting later bring-up.
