# `init_IRQ()` — Architecture IRQ Initialization

## Purpose

Programs the hardware interrupt controller(s) (APIC or legacy PIC on x86) to route interrupts to the appropriate CPU vectors, and sets up the software interrupt routing infrastructure.

## Source File

`arch/x86/kernel/irqinit.c`

## x86 Interrupt Controllers

### Modern: APIC (Advanced Programmable Interrupt Controller)

Modern x86 systems use:
- **Local APIC** (one per CPU): receives interrupts from I/O APIC and sends IPIs
- **I/O APIC** (one or more): receives hardware interrupts (PCI, USB, etc.) and routes them to Local APICs

```
Device IRQ line
    ↓
I/O APIC Redirection Table entry
    ↓ (routes to target CPU)
Local APIC of CPU N
    ↓
CPU interrupt pin
    ↓
IDT vector lookup
    ↓
Interrupt handler
```

### Legacy: 8259A PIC (Programmable Interrupt Controller)

Older systems (and virtualized environments emulating legacy hardware) use two chained 8259A chips:

```
Master 8259A (IRQ 0-7):
  IRQ0 → Timer (PIT)
  IRQ1 → Keyboard
  IRQ2 → Cascade to slave
  IRQ3 → COM2
  IRQ4 → COM1
  IRQ5 → LPT2
  IRQ6 → Floppy
  IRQ7 → LPT1

Slave 8259A (IRQ 8-15):
  IRQ8  → RTC
  IRQ9  → ACPI/free
  IRQ10 → Free/PCI
  IRQ11 → Free/PCI
  IRQ12 → PS/2 Mouse
  IRQ13 → FPU/Coprocessor
  IRQ14 → Primary ATA
  IRQ15 → Secondary ATA
```

## What `init_IRQ()` Does on x86

```c
void __init init_IRQ(void)
{
    int i;
    
    // Fill vectors 32-255 with a "spurious interrupt" handler initially
    for (i = FIRST_EXTERNAL_VECTOR; i < NR_VECTORS; i++) {
        if (!test_bit(i, used_vectors))
            set_intr_gate(i, spurious_interrupt);
    }
    
    // Delegate to platform-specific init:
    x86_init.irqs.intr_init();  // → native_init_IRQ()
}

static void __init native_init_IRQ(void)
{
    // Set up APIC (or PIC in legacy mode)
    apic_intr_mode_init();
    
    // Program I/O APIC redirection table
    // Set up IPIs (Inter-Processor Interrupts)
    // Configure NMI delivery
}
```

## Sub-topics

- [APIC Initialization](apic_init/README.md)
- [Legacy PIC](legacy_pic/README.md)

## IPI Vectors (x86)

IPIs are special interrupts from one CPU to another, used for:

| Vector | Purpose |
|--------|---------|
| `RESCHEDULE_VECTOR` | Tell another CPU to call `schedule()` |
| `CALL_FUNCTION_VECTOR` | Execute a function on another CPU |
| `CALL_FUNCTION_SINGLE_VECTOR` | Execute on a specific CPU |
| `IRQ_MOVE_CLEANUP_VECTOR` | Clean up after IRQ affinity change |
| `REBOOT_VECTOR` | Reboot IPI |
| `X86_PLATFORM_IPI_VECTOR` | Platform-specific |

## Pre-conditions

- `early_irq_init()` complete (irq_desc allocated)
- IDT installed (`trap_init()`)

## Post-conditions

- I/O APIC (or 8259A PIC) programmed
- External interrupt vectors assigned
- IPI vectors programmed in all CPUs' Local APICs
- External hardware interrupts are ready to fire (but IRQs still disabled)

## Cross-references

- [Phase overview](../README.md)
- `early_irq_init()`: [../early_irq_init/README.md](../early_irq_init/README.md)
