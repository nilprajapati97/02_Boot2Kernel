# `smp_prepare_boot_cpu()` — Boot CPU Per-CPU Area Finalization

## Purpose

Performs architecture-specific finalization for the boot CPU's per-CPU data area. On x86-64, this copies the Global Descriptor Table (GDT) and Task State Segment (TSS) into the boot CPU's per-CPU area and updates the CPU's GDT register.

## Source File

`arch/x86/kernel/smp.c` (x86 implementation); `kernel/smp.c` (weak default stub)

## x86-64 Specifics

On x86-64, each CPU has its own:
- **GDT** (Global Descriptor Table) — segment descriptors
- **TSS** (Task State Segment) — used by the CPU on privilege-level switches (ring 3 → ring 0) to find the kernel stack pointer

These must be per-CPU because:
- The TSS has only one `RSP0` field (kernel stack pointer for ring-0 entry)
- Each CPU needs its own kernel stack for interrupt handling

```c
// Simplified x86 smp_prepare_boot_cpu():
void __init smp_prepare_boot_cpu(void)
{
    // Copy template GDT to boot CPU's per-CPU area
    switch_to_new_gdt(smp_processor_id());
    
    // Set up GSBASE to point to boot CPU's per-CPU area
    // (GSBASE used for per_cpu variable access)
    load_percpu_segment(smp_processor_id());
}
```

## The Per-CPU Register (x86-64)

On x86-64, the `GS` segment register base address is used as the per-CPU offset. Code accesses per-CPU variables as:

```c
// this_cpu_read(var) compiles to approximately:
mov %gs:offset_of_var, %rax
```

`smp_prepare_boot_cpu()` sets up the `GSBASE` MSR to point to CPU 0's per-CPU area, activating the per-CPU variable mechanism for the boot CPU.

## Pre-conditions

- `setup_per_cpu_areas()` must have run (per-CPU areas allocated)

## Post-conditions

- Boot CPU's GDT is in the per-CPU area (not the BSS template)
- Boot CPU's TSS is properly configured
- `this_cpu_read()` and `this_cpu_write()` work correctly on the boot CPU

## IRQ State

IRQs **disabled** — modifies CPU descriptor tables.

## Kconfig Dependencies

- Architecture-specific; primarily relevant for `CONFIG_SMP`
- On UP (uniprocessor) builds, this is a no-op

## Cross-references

- [Phase overview](../README.md)
- `setup_per_cpu_areas()`: [../setup_per_cpu_areas/README.md](../setup_per_cpu_areas/README.md)
