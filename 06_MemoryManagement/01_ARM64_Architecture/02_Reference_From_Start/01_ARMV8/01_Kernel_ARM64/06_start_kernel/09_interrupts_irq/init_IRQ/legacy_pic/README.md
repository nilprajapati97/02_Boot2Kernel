# Legacy PIC (8259A) — Programmable Interrupt Controller

## Overview

The Intel 8259A PIC is the classic interrupt controller from the original IBM PC. While modern systems use APIC, the 8259A (or its emulation) may still be present for compatibility.

## Source File

`arch/x86/kernel/i8259.c`

## Two-Chip Cascade

Two 8259A chips are cascaded:

```
Master 8259A              Slave 8259A
INT → CPU                  INT → Master IR2
IR0 → Timer (PIT)          IR0 → RTC
IR1 → Keyboard             IR1 → free
IR2 → Slave cascade        IR2 → free
IR3 → COM2                 IR3 → free
IR4 → COM1                 IR4 → PS/2 mouse (modern)
IR5 → LPT2                 IR5 → free
IR6 → Floppy               IR6 → free
IR7 → LPT1                 IR7 → free (secondary IDE)
```

## Initialization

```c
void __init init_8259A(int auto_eoi)
{
    // ICW1: start init sequence
    outb(0x11, PIC_MASTER_CMD);  // 0x20
    outb(0x11, PIC_SLAVE_CMD);   // 0xA0
    
    // ICW2: set base vectors
    outb(0x20, PIC_MASTER_DATA); // Master starts at vector 0x20 (32)
    outb(0x28, PIC_SLAVE_DATA);  // Slave starts at vector 0x28 (40)
    
    // ICW3: cascade configuration
    outb(0x04, PIC_MASTER_DATA); // Master: slave on IR2
    outb(0x02, PIC_SLAVE_DATA);  // Slave: connected to IR2
    
    // ICW4: 8086 mode
    outb(0x01, PIC_MASTER_DATA);
    outb(0x01, PIC_SLAVE_DATA);
    
    // OCW1: mask all interrupts (APIC will handle them)
    outb(0xFF, PIC_MASTER_DATA);
    outb(0xFF, PIC_SLAVE_DATA);
}
```

## Disabling the PIC When Using APIC

When APIC mode is used, the 8259A must be masked to prevent spurious interrupts:

```c
void __init disable_8259A_irq(unsigned int irq)
{
    // Mask all interrupts
    outb(0xFF, PIC_MASTER_IMR);  // 0x21
    outb(0xFF, PIC_SLAVE_IMR);   // 0xA1
}
```

Some bootloaders may leave the 8259A partially enabled — the kernel always explicitly masks it.

## Spurious IRQ7

IRQ7 from the 8259A can fire spuriously (without a real interrupt source). The kernel detects this by checking the ISR (In-Service Register) — if bit 7 is not set, it's spurious. Spurious IRQ7 increments `irq_err_count` but otherwise ignores it.

## Cross-references

- [init_IRQ parent](../README.md)
- [APIC init](../apic_init/README.md)
