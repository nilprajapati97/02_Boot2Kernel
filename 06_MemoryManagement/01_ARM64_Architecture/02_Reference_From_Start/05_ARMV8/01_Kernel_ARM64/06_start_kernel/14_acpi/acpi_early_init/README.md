# `acpi_early_init()` — ACPI Namespace Initialization

## Purpose

Performs early ACPI initialization: initializes the ACPICA (ACPI Component Architecture) library, loads and parses the ACPI DSDT/SSDT tables, and builds the ACPI namespace tree that represents all hardware devices and power management capabilities.

## Source File

`drivers/acpi/bus.c`

```c
void __init acpi_early_init(void)
{
    acpi_status status;
    
    if (acpi_disabled)
        return;
    
    // Initialize ACPICA subsystems:
    status = acpi_initialize_subsystem();
    if (ACPI_FAILURE(status)) {
        pr_err("Unable to initialize the ACPI Interpreter\n");
        goto error0;
    }
    
    // Load ACPI tables (DSDT, SSDTs) from memory/firmware:
    status = acpi_load_tables();
    if (ACPI_FAILURE(status)) {
        pr_err("Unable to load the System Description Tables\n");
        goto error1;
    }
}
```

## ACPI Tables

The firmware provides ACPI tables in memory, pointed to by RSDP (Root System Description Pointer):

```
RSDP → RSDT/XSDT (Root/Extended System Description Table)
                ↓
        ┌─────────────────────────────────────────────┐
        │ FADT (Fixed ACPI Description Table)          │
        │   → DSDT (Differentiated System Description) │
        │   → FACS (Firmware ACPI Control Structure)  │
        │ SSDT (Secondary System Description Tables)   │
        │ MADT (Multiple APIC Description Table)       │
        │ SRAT (System Resource Affinity Table)        │
        │ SLIT (System Locality Information Table)     │
        │ MCFG (PCI Express Config Space)              │
        │ HPET, WAET, TPM2, IORT, ...                 │
        └─────────────────────────────────────────────┘
```

## AML Interpreter

ACPI uses AML (ACPI Machine Language), a bytecode language embedded in the DSDT:

```
DSDT contains AML code for:
- Device enumeration (_HID, _CID, _UID)
- Power state transitions (_PS0 through _PS3)
- Hardware I/O methods (_INI, _DSM, _CRS)
- Battery, thermal zone, button events
- GPIO, I2C, SPI device descriptions (modern firmware)
```

The kernel includes a complete AML interpreter (ACPICA) that executes this bytecode.

## ACPI Namespace

After loading, ACPI creates an object namespace:

```
\         (root)
├── _SB_  (System Bus)
│   ├── PCI0 (PCI Root Complex)
│   │   ├── GFX0 (GPU)
│   │   └── XHCI (USB controller)
│   ├── LNKB (PCI IRQ link)
│   └── PWRB (Power button)
├── _TZ_  (Thermal Zones)
│   └── THM0 → temperature methods
└── _PR_  (Processor)
    └── CPU0 → P-states, C-states
```

## Disabling ACPI

```bash
# Disable ACPI entirely (use legacy PIC):
acpi=off

# Keep ACPI tables but skip device enumeration:
acpi=noirq

# Skip specific ACPI features:
noapic pci=noacpi
```

## Cross-references

- [Phase overview](../README.md)
- `acpi_subsystem_init()`: [../acpi_subsystem_init/README.md](../acpi_subsystem_init/README.md)
