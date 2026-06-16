# `setup_arch()` — Platform Devices: PCI, ACPI & NUMA

## Overview

A significant portion of `setup_arch()` deals with discovering the platform's hardware topology — NUMA nodes, PCI configuration space, and ACPI tables. This information is needed long before actual device drivers are loaded.

## ACPI Table Discovery

ACPI (Advanced Configuration and Power Interface) tables are stored in memory by the firmware. `setup_arch()` locates them via the RSDP (Root System Description Pointer), which is found either:
- In the BIOS area (0x000E0000–0x000FFFFF)
- In the EFI System Table

### Key ACPI Tables Used During `setup_arch()`

| Table | Description |
|-------|-------------|
| **RSDP** | Root System Description Pointer — entry point |
| **RSDT/XSDT** | Root/Extended System Description Table — table of table pointers |
| **MADT** | Multiple APIC Description Table — lists all CPUs (APIC IDs) |
| **SRAT** | System Resource Affinity Table — NUMA memory/CPU topology |
| **SLIT** | System Locality Information Table — NUMA node distances |
| **MCFG** | PCI Memory-mapped Configuration Space addresses |

### What is Read from MADT

`acpi_boot_parse_madt()` reads the MADT to:
1. Find all Local APIC entries → populates `cpu_possible_mask`
2. Find I/O APIC entries → needed for `init_IRQ()`
3. Find interrupt source overrides → IRQ routing information
4. Find NMI sources

### What is Read from SRAT/SLIT

`acpi_numa_init()` reads SRAT to build the NUMA topology:
- Maps APIC IDs to NUMA node numbers
- Maps physical memory ranges to NUMA nodes
- Reads SLIT to get inter-node distances (used by the scheduler and memory allocator)

## PCI Early Setup

Before any PCI drivers load, `setup_arch()` initializes PCI configuration space access:

### PCI Configuration Space Access Methods (x86)

| Method | When Used |
|--------|-----------|
| **Port I/O** (CF8/CFC) | Legacy; always available on x86 |
| **MMCONFIG** | Modern; maps config space to virtual memory; addresses from MCFG table |

```c
// Early PCI init in setup_arch():
pci_mmconfig_early_init();   // Map MMCONFIG from MCFG ACPI table
```

### PCI Bus Scanning (NOT done here)

Full PCI bus scanning (assigning BARs, loading drivers) happens much later via `pci_subsys_init()` → `pcibios_init()` in the initcall sequence. During `setup_arch()`, only the configuration space *access mechanism* is set up.

## NUMA Initialization

NUMA (Non-Uniform Memory Access) support requires:
1. Knowing which physical memory ranges belong to which NUMA node
2. Allocating `struct pglist_data` (zone descriptor) per node
3. Sizing the per-node page arrays

All of this is done during `setup_arch()` using `memblock` for allocations:

```
numa_init()
  ├── acpi_numa_init()          // Read SRAT/SLIT
  ├── numa_register_nodes()     // Set up node_data[] per NUMA node
  └── setup_node_to_cpumask_map() // Build node→CPU mapping
```

After this, `NODE_DATA(nid)` returns the `pglist_data` for NUMA node `nid`.

## Result

After the platform device portion of `setup_arch()`:
- Number of CPUs and their APIC IDs are known
- NUMA node count, memory ranges, and distances are known
- PCI configuration space access is initialized
- `memblock` is reserved for all ACPI table memory

## Cross-references

- [setup_arch overview](../README.md)
- `acpi_early_init()` — full ACPI namespace: [../../../14_acpi/acpi_early_init/README.md](../../../14_acpi/acpi_early_init/README.md)
- `init_IRQ()` — uses MADT info: [../../../09_interrupts_irq/init_IRQ/README.md](../../../09_interrupts_irq/init_IRQ/README.md)
