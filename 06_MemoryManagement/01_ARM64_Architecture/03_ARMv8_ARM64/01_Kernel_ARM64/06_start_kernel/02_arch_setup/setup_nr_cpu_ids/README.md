# `setup_nr_cpu_ids()` — Compute CPU Count

## Purpose

Computes `nr_cpu_ids` — the number of possible CPUs — by finding the highest set bit in `cpu_possible_mask` and adding 1. This value is used throughout the kernel to size per-CPU arrays and iteration bounds.

## Source File

`kernel/smp.c`

```c
void __init setup_nr_cpu_ids(void)
{
    nr_cpu_ids = find_last_bit(cpumask_bits(cpu_possible_mask),
                               NR_CPUS) + 1;
}
```

## Why This Matters

Many kernel data structures are sized by `nr_cpu_ids` at boot:
- `cpu_possible_mask` itself (sized to `NR_CPUS` at compile time, but `nr_cpu_ids` tightens the bounds)
- IRQ affinity masks
- Scheduler topology arrays
- Memory subsystem per-CPU lists

Using `nr_cpu_ids` instead of `NR_CPUS` (the compile-time maximum) saves memory on systems with fewer CPUs than the compile-time maximum.

## Pre-conditions

- `setup_arch()` must have populated `cpu_possible_mask` via ACPI MADT scanning

## Post-conditions

- `nr_cpu_ids` is set to the correct number of possible CPUs

## IRQ State

IRQs **disabled** — simple arithmetic.

## Cross-references

- [Phase overview](../README.md)
