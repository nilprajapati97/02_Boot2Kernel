# APIC Initialization — x86 Interrupt Controller Setup

## Source File

`arch/x86/kernel/apic/apic.c`, `arch/x86/kernel/apic/io_apic.c`

## Local APIC Setup

Each CPU has its own Local APIC (memory-mapped at 0xFEE00000 by default or via x2APIC MSR):

```c
void __init apic_intr_mode_init(void)
{
    // Determine APIC mode from cmdline and hardware capabilities:
    // - x2APIC if supported and enabled
    // - xAPIC otherwise
    
    apic_intr_mode_select();
    
    // Enable APIC in BSP (Boot Strap Processor = CPU 0):
    enable_IR_x2apic();  // or enable_xapic()
    
    // Set spurious interrupt vector
    apic_write(APIC_SPIV, APIC_SPIV_APIC_ENABLED | SPURIOUS_APIC_VECTOR);
    
    // Set up LVT (Local Vector Table) entries:
    apic_write(APIC_LVTT, LOCAL_TIMER_VECTOR);  // Local timer
    apic_write(APIC_LVT0, APIC_DM_EXTINT);      // PIC cascade (CPU 0 only)
    apic_write(APIC_LVT1, APIC_DM_NMI);         // NMI
}
```

## Local APIC Registers

| Offset | Register | Description |
|--------|----------|-------------|
| 0x020 | ID | Local APIC ID |
| 0x080 | TPR | Task Priority Register (IRQ threshold) |
| 0x0B0 | EOI | End Of Interrupt (write to acknowledge) |
| 0x0D0 | LDR | Logical Destination Register |
| 0x0E0 | DFR | Destination Format Register |
| 0x0F0 | SVR | Spurious Vector Register (enable/disable) |
| 0x100-0x170 | ISR | In-Service Register |
| 0x180-0x1F0 | TMR | Trigger Mode Register |
| 0x200-0x270 | IRR | Interrupt Request Register |
| 0x280 | ESR | Error Status Register |
| 0x300 | ICR_LOW | Interrupt Command Register (for IPIs) |
| 0x310 | ICR_HIGH | IPI target CPU |
| 0x320 | LVT_TIMER | Local Timer configuration |
| 0x350 | LVT_LINT0 | LINT0 (legacy PIC cascade) |
| 0x360 | LVT_LINT1 | LINT1 (NMI) |

## I/O APIC Redirection Table

Each I/O APIC has a **Redirection Table** (RTE) with one entry per interrupt pin:

```
I/O APIC Pin 0 (IRQ0 = timer):
  RTE[0]: Vector=0x30, Destination=CPU0, Trigger=Edge, Polarity=Active-High

I/O APIC Pin 1 (IRQ1 = keyboard):
  RTE[1]: Vector=0x31, Destination=CPU0, Trigger=Edge, Polarity=Active-High
```

Programming an RTE:
```c
// Route IRQ to CPU, set vector, trigger mode:
io_apic_setup_irq_pin(irq, cpu, trigger, polarity);
```

## x2APIC Mode

x2APIC is an extension that:
- Uses MSR interface instead of MMIO (faster)
- Supports more than 255 CPUs (32-bit APIC IDs)
- Required for systems with >255 logical CPUs

```
xAPIC:  MMIO at 0xFEE00000
x2APIC: RDMSR/WRMSR 0x800-0x8FF
```

## Cross-references

- [init_IRQ parent](../README.md)
- [Legacy PIC](../legacy_pic/README.md)
