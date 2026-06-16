# `setup_per_cpu_areas()` — Per-CPU Section Replication

## Purpose

Allocates a separate copy of the kernel's `.data..percpu` section for each possible CPU and sets up the per-CPU base pointers. This is the foundation for the per-CPU variable mechanism, which allows each CPU to have its own independent copy of a variable without any locking.

## Source File

`mm/percpu.c` (generic); arch may override in `arch/x86/kernel/setup_percpu.c`

## How Per-CPU Variables Work

In the kernel source, per-CPU variables are declared like:

```c
DEFINE_PER_CPU(int, my_counter);          // One int per CPU
DEFINE_PER_CPU(struct runqueue, runqueues); // One runqueue per CPU
```

At compile time, all these go into a special ELF section: `.data..percpu`.

At runtime, each CPU needs its own copy. `setup_per_cpu_areas()` creates these copies:

```
vmlinux .data..percpu (template):
  [my_counter][runqueues][other_percpu_vars]
  
After setup_per_cpu_areas():
  CPU 0 area: [my_counter][runqueues][other_percpu_vars]
  CPU 1 area: [my_counter][runqueues][other_percpu_vars]
  CPU 2 area: [my_counter][runqueues][other_percpu_vars]
  ...
```

Each CPU has a `per_cpu_offset[cpu]` value. Accessing `per_cpu(my_counter, 2)` adds `per_cpu_offset[2]` to the template address.

The current CPU's offset is stored in a dedicated register on some architectures (e.g., `GS` segment base on x86-64), making `this_cpu_read(my_counter)` a single instruction with no function call overhead.

## Allocation Strategy

`setup_per_cpu_areas()` uses `memblock_alloc_node()` to allocate per-CPU areas. On NUMA systems, it tries to allocate each CPU's area on its local NUMA node to minimize memory access latency.

The allocation must happen early (before `mm_core_init()`) because the page allocator itself uses per-CPU data structures (hot/cold page lists, pcplists).

## Pre-conditions

- `memblock` must have enough free memory
- `nr_cpu_ids` must be known
- NUMA topology must be set up (if NUMA is enabled)

## Post-conditions

- `per_cpu_offset[]` array is set for all possible CPUs
- `__per_cpu_start` / `__per_cpu_end` mark the template section
- Boot CPU's GS base (x86) is set to its per-CPU area

## IRQ State

IRQs **disabled** — memblock allocation.

## Key Data Structures

| Symbol | Purpose |
|--------|---------|
| `per_cpu_offset[]` | Base address of each CPU's per-CPU area |
| `pcpu_base_addr` | Base of all per-CPU allocations |
| `__per_cpu_start` | Linker symbol: start of per-CPU template |
| `__per_cpu_end` | Linker symbol: end of per-CPU template |

## Kconfig Dependencies

- `CONFIG_SMP`: Enables multiple per-CPU areas; without it, a single area is used
- `CONFIG_NUMA`: Enables NUMA-aware allocation
- `CONFIG_HAVE_SETUP_PER_CPU_AREA`: Arch flag to use arch-specific setup

## Cross-references

- [Phase overview](../README.md)
- `setup_per_cpu_pageset()` — later per-CPU page setup: [../../13_console_locking/../README.md](../../README.md)
