# `early_irq_init()` — IRQ Descriptor Table Allocation

## Purpose

Allocates the kernel's IRQ descriptor table — an array of `struct irq_desc` objects, one per interrupt number. This is the kernel's software representation of the interrupt system, separate from the hardware IDT.

## Source File

`kernel/irq/irqdesc.c`

## Two Layers of Interrupt Abstraction

Linux has two separate interrupt abstractions:

### Layer 1: Hardware IDT (x86-specific)
- 256 entries indexed by CPU vector number (0–255)
- Maps CPU exception vectors to assembly entry points
- Set up by `trap_init()` in Phase 4

### Layer 2: Linux IRQ Numbers + `irq_desc`
- Logical IRQ numbers (0–N, can be thousands)
- Maps to hardware interrupt lines via `irq_chip` + `irq_domain`
- Set up here by `early_irq_init()`

## `struct irq_desc`

```c
struct irq_desc {
    struct irq_common_data  irq_common_data;
    struct irq_data         irq_data;       // chip, domain, hwirq
    unsigned int __percpu  *kstat_irqs;     // Per-CPU stats
    irq_flow_handler_t      handle_irq;     // High-level handler
    struct irqaction       *action;         // Registered handlers
    unsigned int            status_use_accessors;
    unsigned int            core_internal_state__do_not_mess_with_it;
    unsigned int            depth;          // Disable depth
    unsigned int            wake_depth;     // Wake enable depth
    unsigned int            tot_count;
    unsigned int            irq_count;      // For spurious detection
    unsigned long           last_unhandled;
    unsigned int            irqs_unhandled;
    atomic_t                threads_handled;
    int                     threads_handled_last;
    raw_spinlock_t          lock;
    struct cpumask         *percpu_enabled;
    const struct cpumask   *percpu_affinity;
#ifdef CONFIG_SMP
    const struct cpumask   *affinity_hint;
    struct irq_affinity_notify *affinity_notify;
#endif
    /* ... many more ... */
};
```

## Allocation

```c
int __init early_irq_init(void)
{
    // Allocate NR_IRQS descriptors (typically 256 or 4096)
    for (i = 0; i < count; i++) {
        desc[i].kstat_irqs = alloc_percpu(unsigned int);
        init_irq_desc(&desc[i], i);
        lockdep_set_class(&desc[i].lock, &irq_desc_lock_class);
    }
    
    arch_early_irq_init();
}
```

## IRQ Domain: Hardware to Linux IRQ Mapping

Modern kernels use **IRQ domains** to map hardware interrupt numbers to Linux IRQ numbers:

```
Hardware:               IRQ Domain:              Linux:
PCI MSI interrupt  →   pci_msi_domain_ops   →   IRQ 45
APIC vector 0x30   →   x86_vector_domain    →   IRQ 32
GIC interrupt 123  →   gic_irq_domain_ops   →   IRQ 200
```

This allows systems with multiple interrupt controllers, virtualization, and MSI to work coherently.

## Pre-conditions

- `kmalloc()` and `alloc_percpu()` available

## Post-conditions

- `irq_desc[]` array allocated
- IRQ numbers 0 to `NR_IRQS-1` have descriptors
- `irq_to_desc(irq)` functional

## Cross-references

- [Phase overview](../README.md)
- `init_IRQ()`: [../init_IRQ/README.md](../init_IRQ/README.md)
