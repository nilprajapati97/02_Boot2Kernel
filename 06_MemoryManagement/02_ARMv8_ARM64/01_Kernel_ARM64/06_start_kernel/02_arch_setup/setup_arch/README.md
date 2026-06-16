# `setup_arch()` — Architecture-Specific Initialization

## Purpose

The largest and most complex function called from `start_kernel()`. It performs all architecture-specific initialization required before the generic kernel can proceed: memory map detection, CPU feature identification, early page table setup, ACPI table loading, and much more.

## Source File

`arch/x86/kernel/setup.c` (for x86-64; other architectures have their own)

## Signature

```c
void __init setup_arch(char **cmdline_p)
```

The `cmdline_p` output parameter is set to point to the kernel command line string as found by the architecture code (e.g., from `boot_params.hdr.cmd_line_ptr` on x86).

## What `setup_arch()` Does on x86-64

The function is approximately 400 lines on x86-64 and calls dozens of sub-functions. At a high level:

### 1. Command Line & Boot Params
- Reads `boot_params` (filled by the bootloader per the x86 boot protocol)
- Copies the command line to `boot_command_line[]`
- Sets `*cmdline_p` to point to it

### 2. Memory Map (e820)
- Reads the e820 memory map from `boot_params.e820_table`
- Calls `e820__memory_setup()` to process and sanitize the map
- Populates `memblock` with available RAM regions
- Reserves regions for: kernel image, initrd, BIOS, ACPI tables, PCI BARs

See [memory_map/README.md](memory_map/README.md) for details.

### 3. CPU Features
- Calls `early_cpu_init()` → identifies vendor (Intel/AMD/etc.)
- Reads CPUID to detect feature flags (SSE, AVX, TSC, etc.)
- Applies CPU-specific errata workarounds
- Sets up `boot_cpu_data` (`struct cpuinfo_x86`)

See [cpu_detection/README.md](cpu_detection/README.md) for details.

### 4. Kernel Virtual Address Layout
- Establishes the kernel direct mapping range
- Sets up `vmalloc` area boundaries
- On 5-level paging (x86-64 with `CONFIG_X86_5LEVEL`), enables LA57 if available

### 5. KASLR (Kernel Address Space Layout Randomization)
- If `CONFIG_RANDOMIZE_BASE` is set and not disabled via `nokaslr`, the kernel image is already positioned randomly (done in compressed kernel before `start_kernel`)
- `setup_arch()` finalizes the offset

### 6. SMP Initialization Prep
- Detects NUMA topology from ACPI SRAT / SLIT tables
- Initializes `node_data[]` for NUMA nodes
- Calls `smp_init_cpus()` to scan ACPI MADT for available CPUs

See [platform_devices/README.md](platform_devices/README.md) for details.

### 7. PCI & Platform Early Setup
- Calls `early_pci_allowed()` and `pci_early_idt_handler_init()`
- Initializes PCI config space access methods (port I/O or MMCONFIG)

### 8. EFI
- If booted via UEFI: maps EFI runtime services, reads EFI variables
- Checks Secure Boot status (may call into early LSM)

### 9. Page Table Setup
- Calls `init_mem_mapping()` — creates the kernel direct mapping (physmem → `PAGE_OFFSET`)
- Sets up `vmemmap` (maps `struct page[]` array)

## Pre-conditions

- `boot_params` filled by bootloader
- `memblock` partially initialized with static kernel regions
- Early LSMs initialized

## Post-conditions

- `*cmdline_p` points to kernel command line
- `memblock` fully describes all RAM and reservations
- `boot_cpu_data` describes the boot CPU
- NUMA topology is known
- Kernel direct mapping is complete
- `nr_cpu_ids` is computed (or approximated)

## IRQ State

IRQs **disabled** throughout.

## Key Data Structures

| Symbol | Type | Purpose |
|--------|------|---------|
| `boot_params` | `struct boot_params` | x86 boot protocol data from bootloader |
| `boot_cpu_data` | `struct cpuinfo_x86` | CPU feature flags and identification |
| `e820_table` | `struct e820_table` | Physical memory layout from BIOS |
| `memblock` | `struct memblock` | Kernel's early memory allocator |
| `numa_meminfo` | `struct numa_meminfo` | NUMA node memory ranges |

## Kconfig Dependencies

- `CONFIG_X86_64` vs `CONFIG_X86_32`: Major differences
- `CONFIG_NUMA`: NUMA topology detection
- `CONFIG_ACPI`: ACPI table parsing during setup_arch
- `CONFIG_EFI`: EFI runtime services mapping
- `CONFIG_RANDOMIZE_BASE`: KASLR support

## Sub-topics

- [memory_map/README.md](memory_map/README.md) — e820 map, memblock, physical memory layout
- [cpu_detection/README.md](cpu_detection/README.md) — CPUID, feature flags, microcode
- [platform_devices/README.md](platform_devices/README.md) — PCI, ACPI, NUMA topology

## Cross-references

- [Phase overview](../README.md)
- `arch_cpu_finalize_init()` — CPU finalization much later: [../../14_acpi/arch_cpu_finalize_init/README.md](../../14_acpi/arch_cpu_finalize_init/README.md)
