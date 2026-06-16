# `smp_setup_processor_id()` — Boot CPU Identification

## Purpose

Records the hardware CPU ID (e.g., APIC ID on x86) of the boot processor and associates it with logical CPU 0. This establishes the mapping between hardware CPU identity and the Linux logical CPU numbering scheme used by `smp_processor_id()`.

## Source File

`kernel/smp.c` (weak stub — arch overrides in `arch/x86/kernel/smp.c` or similar)

```c
void __init __weak smp_setup_processor_id(void)
{
    /* Default: do nothing. Arch overrides as needed. */
}
```

On x86, the arch code reads the Local APIC ID from `cpuid` or APIC registers to identify the boot processor.

## Why This Matters

Linux uses two CPU numbering schemes:

| Scheme | Name | Example |
|--------|------|---------|
| **Logical** | `cpu` or `smp_processor_id()` | 0, 1, 2, 3, ... |
| **Physical** | APIC ID / MPIDR (ARM) | 0, 2, 4, 6 (may have gaps) |

The mapping between them is established by `smp_setup_processor_id()`. Without it, `smp_processor_id()` would return garbage, breaking every per-CPU variable access.

## x86 Implementation Detail

On x86-64 (in `arch/x86/kernel/setup.c` / SMP code):
- Reads the APIC ID from `cpuid(0xB)` (x2APIC) or the APIC MMIO register
- Stores it in `early_per_cpu(x86_cpu_to_apicid, 0)`
- Sets `boot_cpu_physical_apicid`

## Pre-conditions

- CPU is identifiable (CPUID instruction works)

## Post-conditions

- `smp_processor_id()` returns 0 for the boot CPU
- The logical-to-physical CPU ID mapping is initialized for CPU 0

## IRQ State

IRQs may be on or off — this is a read-only CPU identification operation.

## Key Data Structures

| Variable | Type | Purpose |
|----------|------|---------|
| `boot_cpu_physical_apicid` | `int` (x86) | Hardware APIC ID of boot CPU |
| `x86_bios_cpu_apicid` | per-CPU | BIOS-assigned APIC IDs |

## Kconfig Dependencies

- `CONFIG_SMP`: If not set, a simplified stub is used
- `CONFIG_X86_LOCAL_APIC`: Enables APIC-based CPU identification on x86

## Cross-references

- [Phase overview](../README.md)
- `boot_cpu_init()` — uses the CPU ID set here: [../boot_cpu_init/README.md](../boot_cpu_init/README.md)
