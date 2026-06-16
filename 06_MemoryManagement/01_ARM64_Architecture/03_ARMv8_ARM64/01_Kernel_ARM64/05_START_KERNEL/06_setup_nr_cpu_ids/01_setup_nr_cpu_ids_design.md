# 01. setup_nr_cpu_ids Design

## Overview

`setup_nr_cpu_ids()` is a generic Linux kernel boot-time helper that establishes the valid range of CPU IDs for the current system boot. It does not itself enumerate CPUs; instead, it derives the maximum CPU ID from the architecture-provided `cpu_possible_mask` and sets the global `nr_cpu_ids` limit.

This is important because Linux uses several CPU-related limits:

- `NR_CPUS`: compile-time upper bound for CPU IDs
- `nr_cpu_ids`: runtime configured number of valid CPU IDs
- `setup_max_cpus`: runtime target maximum CPUs to activate
- `cpu_possible_mask`: possible CPU ID bitmap for this platform
- `cpu_present_mask`: actual CPUs populated at boot or by hotplug
- `cpu_online_mask`: CPUs available to scheduler

## Why the function exists

`setup_nr_cpu_ids()` exists so the kernel can:

1. determine the effective maximum CPU ID it will use for this boot
2. make runtime CPU masks and arrays only valid for that many CPUs
3. allow architecture code to set a smaller limit than `NR_CPUS`
4. support boot-time options like `nr_cpus=` and platform enumeration results

Without it, the kernel would either have to assume the full static `NR_CPUS` range or risk invalid CPU-ID arithmetic.

## Core data structures

### `NR_CPUS`

- static compile-time maximum
- defines the size of `cpumask_t` bitmaps and many static arrays
- usually much larger than the real CPU count on a platform

### `nr_cpu_ids`

- runtime maximum CPU ID limit
- valid values are `<= NR_CPUS`
- used by cpumask macros and CPU-ID validation
- controls which CPU IDs are considered possible or allowable

### `cpu_possible_mask`

- a bitmap of CPU IDs that may ever exist for this boot
- set by architecture/platform code during early boot
- only `nr_cpu_ids` bits are meaningful

### `setup_max_cpus`

- runtime target from boot command line (`maxcpus=`)
- controls how many CPUs may be activated during SMP bring-up
- not the same as `nr_cpu_ids` but must be `<= nr_cpu_ids`

## Function definition

The generic implementation in `linux/kernel/smp.c` is:

```c
void __init setup_nr_cpu_ids(void)
{
    set_nr_cpu_ids(find_last_bit(cpumask_bits(cpu_possible_mask), NR_CPUS) + 1);
}
```

### Step-by-step breakdown

1. `cpumask_bits(cpu_possible_mask)` returns the raw bitmap array for the mask.
2. `find_last_bit(..., NR_CPUS)` scans the bitmap to find the highest set CPU bit.
3. `+ 1` converts the highest set CPU number into a count/limit.
4. `set_nr_cpu_ids(...)` stores that value in `nr_cpu_ids`.

If the highest set CPU is ID 3, `nr_cpu_ids` becomes `4`, meaning valid IDs are `0..3`.

## Top-down boot flow

### `start_kernel()`

At the point of `setup_nr_cpu_ids()` in `start_kernel()`:

- architecture setup has completed with a valid `cpu_possible_mask`
- boot command-line parsing has not yet processed all options that affect CPU boot behavior
- `setup_nr_cpu_ids()` establishes the static runtime CPU ID limit before per-CPU memory setup

### Why it is here

`setup_nr_cpu_ids()` must run before:

- `setup_per_cpu_areas()`
- `smp_prepare_boot_cpu()`
- any per-CPU allocation that depends on `nr_cpu_ids`

That way the kernel can size CPU masks and per-CPU lookup tables correctly.

## What it does not do

`setup_nr_cpu_ids()` does not:

- physically boot secondary CPUs
- allocate per-CPU data structures
- parse `maxcpus=` or `nr_cpus=` itself
- inspect hardware topology beyond the maximum CPU ID bit

## How boot options interact

Boot parameters influence the value of `nr_cpu_ids` indirectly before or during `setup_nr_cpu_ids()`:

- `nr_cpus=<n>` can reduce the configured CPU count below the platform maximum
- `maxcpus=<n>` sets `setup_max_cpus`, limiting actual bring-up later
- `nosmp` sets `setup_max_cpus = 0` and may disable SMP support entirely

These are handled by early parameter hooks in `linux/kernel/smp.c` and platform code.

## Expandable pseudo-code view

From scratch, the logic looks like:

```c
void __init setup_nr_cpu_ids(void)
{
    unsigned int highest_cpu;

    // The arch/platform code defines which cpu ids are possible.
    // It might be from device tree, ACPI MADT, bootloader data, or hardcoded defaults.
    highest_cpu = find_last_bit(cpumask_bits(cpu_possible_mask), NR_CPUS);

    // If no bit is set, highest_cpu is NR_CPUS, but cpu_possible_mask must always
    // contain at least the boot CPU (CPU 0) on SMP-capable systems.

    set_nr_cpu_ids(highest_cpu + 1);
}
```

### Important invariant

- `nr_cpu_ids` must always be at least `1` on SMP-capable systems
- `cpu_possible_mask` should include the boot CPU
- `nr_cpu_ids` must never exceed `NR_CPUS`

## Validation and error handling

There is minimal runtime checking here because the architecture code is expected to have already built a valid possible mask. If the mask is malformed, later CPU-setup and mask operations will catch it.

## Relationship with `set_nr_cpu_ids()`

`set_nr_cpu_ids()` is a helper in `include/linux/cpumask.h` that only updates the runtime limit when the build supports it.

On UP systems or when `CONFIG_FORCE_NR_CPUS` is defined, the call becomes a no-op or a warning because `nr_cpu_ids` is fixed at compile time.

## Summary

`setup_nr_cpu_ids()` is a small but critical boot-time bridge between architecture/platform CPU enumeration and the generic kernel SMP framework. It converts a platform-specific bitmap of possible CPUs into the kernel-wide runtime CPU ID limit `nr_cpu_ids`, ensuring that later per-CPU allocations, CPU masks, and SMP bring-up use the correct CPU range.
