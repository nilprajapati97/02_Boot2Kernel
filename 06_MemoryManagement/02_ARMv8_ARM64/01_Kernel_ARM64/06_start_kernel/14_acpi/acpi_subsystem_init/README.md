# `acpi_subsystem_init()` — Full ACPI Bus Initialization

## Purpose

Completes ACPI initialization by enabling the ACPI hardware interface, running `_INI` and `_STA` methods on all devices to determine their presence and state, and installing ACPI event handlers.

## Source File

`drivers/acpi/bus.c`

```c
void __init acpi_subsystem_init(void)
{
    acpi_status status;
    
    if (acpi_disabled)
        return;
    
    // Enable ACPI hardware mode (transition from legacy):
    status = acpi_enable();
    if (ACPI_FAILURE(status)) {
        // System may still function with ACPI tables only
        pr_err("Failed to enable ACPI\n");
        return;
    }
    
    // Install ACPI event handlers (power button, sleep button, etc.):
    acpi_install_notify_handler(ACPI_ROOT_OBJECT, ACPI_SYSTEM_NOTIFY,
                                 acpi_bus_notify, NULL);
    
    // Run ACPI initialization methods (_INI) on all objects:
    acpi_initialize_objects(ACPI_FULL_INITIALIZATION);
}
```

## ACPI Enable Transition

Before `acpi_enable()`, the system is in "legacy mode":
- IRQs routed through 8259A PIC
- Hardware controlled by legacy IO ports

After `acpi_enable()`:
- IRQs routed through APIC (if available)
- SCI (System Control Interrupt) registered
- SMI_CMD port written to switch firmware to ACPI mode

```
FADT.SMI_CMD port ← FADT.ACPI_ENABLE value
    → Firmware transfers hardware control to OS
```

## `_INI` Methods

Each ACPI device can have an `_INI` (Initialize) method that runs board-specific hardware setup:

```asm
/* AML example from a real DSDT: */
Method (_INI, 0, NotSerialized) {
    Store (0x01, \_SB.PCI0.LPCB.EC.XXXX)  // Write to EC register
    If (LEqual (OSFL (), Zero)) {           // If Windows
        Store (0x02, SMPR)
    }
}
```

## `_STA` Methods

Each device can have a `_STA` (Status) method returning a bitmask:

```
Bit 0: Device is present
Bit 1: Device is enabled (I/O assigned)
Bit 2: Device should be shown in UI
Bit 3: Device is functioning
Bit 4: Battery present (batteries only)
```

## ACPI Power States

ACPI defines system-wide power states (S-states):

| State | Name | Description |
|-------|------|-------------|
| S0 | Working | Normal operation |
| S1 | Sleep (deprecated) | CPU stopped, RAM powered |
| S2 | Sleep (deprecated) | S1 + CPU/cache power off |
| S3 | Suspend to RAM | RAM only powered |
| S4 | Suspend to Disk (hibernate) | State saved to disk |
| S5 | Soft Off | Off, but PSU powered (wake possible) |
| G3 | Mechanical Off | Fully power off |

And device power states D0-D3:

| State | Power | Latency |
|-------|-------|---------|
| D0 | Full | 0 |
| D1 | Medium | Short |
| D2 | Low | Medium |
| D3hot | Off (but power) | Long |
| D3cold | No power | Longest |

## Cross-references

- [Phase overview](../README.md)
- `acpi_early_init()`: [../acpi_early_init/README.md](../acpi_early_init/README.md)
- `init_IRQ()`: [../../09_interrupts_irq/init_IRQ/README.md](../../09_interrupts_irq/init_IRQ/README.md) — APIC setup
