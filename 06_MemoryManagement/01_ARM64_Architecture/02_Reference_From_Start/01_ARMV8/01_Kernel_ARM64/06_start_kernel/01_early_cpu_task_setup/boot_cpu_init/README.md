# `boot_cpu_init()` — CPU Bitmask Initialization

## Purpose

Sets the boot CPU (CPU 0) in all four of the kernel's global CPU bitmasks: `possible`, `present`, `online`, and `active`. Without this, iteration macros like `for_each_online_cpu()` would skip the boot CPU.

## Source File

`kernel/cpu.c`

```c
static void __init boot_cpu_init(void)
{
    int cpu = smp_processor_id();

    /* Mark the boot cpu "present", "online" etc for SMP and UP case */
    set_cpu_online(cpu, true);
    set_cpu_active(cpu, true);
    set_cpu_present(cpu, true);
    set_cpu_possible(cpu, true);

#ifdef CONFIG_SMP
    __boot_cpu_id = cpu;
#endif
}
```

## The Four CPU Bitmasks

Linux tracks CPU state in four bitmasks. Each is a `cpumask_t` (bitmap with one bit per possible CPU):

| Bitmask | Meaning | When Bit is Set |
|---------|---------|-----------------|
| `cpu_possible_mask` | CPU could ever exist in this system | At boot; never cleared |
| `cpu_present_mask` | CPU physically present (may be offline) | Hardware present; cleared on physical removal |
| `cpu_online_mask` | CPU is online and processing IRQs | After `cpu_up()`; cleared by `cpu_down()` |
| `cpu_active_mask` | CPU participates in load balancing | After scheduler is ready; cleared during hotplug |

**Relationship:**
```
active ⊆ online ⊆ present ⊆ possible
```

## Why All Four at Boot?

The boot CPU goes through a compressed form of the normal hotplug sequence. For the boot CPU, all four states are set simultaneously at boot rather than incrementally.

## Key Macros (after this call)

```c
for_each_possible_cpu(cpu)  // iterates {0}
for_each_present_cpu(cpu)   // iterates {0}
for_each_online_cpu(cpu)    // iterates {0}
for_each_active_cpu(cpu)    // iterates {0}
```

## Pre-conditions

- `smp_processor_id()` must return the correct boot CPU ID (set by `smp_setup_processor_id()`)

## Post-conditions

- Boot CPU bit is set in all four cpumasks
- `__boot_cpu_id` records the boot CPU (SMP only)
- `for_each_online_cpu()` will include the boot CPU

## IRQ State

IRQs **disabled** — called after `local_irq_disable()`.

## Key Data Structures

| Symbol | Type | Defined in |
|--------|------|-----------|
| `cpu_possible_mask` | `cpumask_t` | `kernel/cpu.c` |
| `cpu_present_mask` | `cpumask_t` | `kernel/cpu.c` |
| `cpu_online_mask` | `cpumask_t` | `kernel/cpu.c` |
| `cpu_active_mask` | `cpumask_t` | `kernel/cpu.c` |

## Kconfig Dependencies

- `CONFIG_SMP`: Enables `__boot_cpu_id` and multi-CPU mask support
- `CONFIG_HOTPLUG_CPU`: Enables runtime CPU hot-add/remove

## Cross-references

- [Phase overview](../README.md)
- `smp_setup_processor_id()` — sets `smp_processor_id()` return value: [../smp_setup_processor_id/README.md](../smp_setup_processor_id/README.md)
- `smp_init()` in `19_rest_init` — brings secondary CPUs online
