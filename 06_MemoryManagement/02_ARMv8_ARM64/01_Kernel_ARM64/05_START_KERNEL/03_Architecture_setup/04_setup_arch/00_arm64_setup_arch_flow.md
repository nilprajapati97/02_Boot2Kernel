Here’s a README-style note you can save as:

```text
arm64_setup_arch_flow.md
```

# `setup_arch(&command_line)` on ARMv8 / ARM64

`setup_arch(&command_line)` is called from `start_kernel()` during early Linux boot. On ARM64, it performs **architecture-specific initialization** before the generic kernel subsystems continue.

Your earlier notes focused on early kernel logging with `pr_notice()` and `printk()` . This function is the next major boot-stage setup point.

---

## Function prototype

```c
void __init setup_arch(char **cmdline_p)
```

Called like:

```c
setup_arch(&command_line);
```

Meaning:

```c
cmdline_p = &command_line;
```

So ARM64 setup can assign the final boot command line:

```c
*cmdline_p = boot_command_line;
```

In current ARM64 kernel source, `setup_arch()` sets initial `init_mm`, assigns `boot_command_line`, initializes KASLR, fixmap, early ioremap, parses FDT, initializes memory, paging, ACPI/DT, PSCI, CPU topology, and MPIDR mapping. ([GitHub][1])

---

# High-Level Flow Chart

```text
start_kernel()
    |
    v
setup_arch(&command_line)
    |
    +--> setup_initial_init_mm()
    |
    +--> command_line = boot_command_line
    |
    +--> kaslr_init()
    |
    +--> early_fixmap_init()
    |
    +--> early_ioremap_init()
    |
    +--> setup_machine_fdt(__fdt_pointer)
    |       |
    |       +--> map DTB using fixmap
    |       +--> early_init_dt_scan()
    |       +--> reserve DTB memory
    |       +--> read machine model
    |
    +--> jump_label_init()
    |
    +--> parse_early_param()
    |
    +--> local_daif_restore()
    |
    +--> cpu_uninstall_idmap()
    |
    +--> xen_early_init()
    |
    +--> efi_init()
    |
    +--> arm64_memblock_init()
    |
    +--> paging_init()
    |
    +--> acpi_boot_table_init()
    |
    +--> unflatten_device_tree()   [if ACPI disabled]
    |
    +--> bootmem_init()
    |
    +--> kasan_init()
    |
    +--> request_standard_resources()
    |
    +--> early_ioremap_reset()
    |
    +--> psci_dt_init() / psci_acpi_init()
    |
    +--> init_bootcpu_ops()
    |
    +--> smp_init_cpus()
    |
    +--> smp_build_mpidr_hash()
    |
    +--> check boot_args x1-x3
```

---

# Structure-Wise Explanation

## 1. Initial kernel memory layout

```c
setup_initial_init_mm(_text, _etext, _edata, _end);
```

This initializes `init_mm`, the first memory descriptor used by the kernel.

Conceptually:

```text
_text   -> kernel code start
_etext  -> kernel code end
_edata  -> kernel data end
_end    -> kernel image end
```

---

## 2. Command line setup

```c
*cmdline_p = boot_command_line;
```

This connects the generic kernel variable:

```c
command_line
```

to the architecture-prepared:

```c
boot_command_line
```

The command line may come from:

```text
bootloader
    |
    v
Device Tree / EFI / boot protocol
    |
    v
boot_command_line
    |
    v
command_line
```

---

## 3. Early virtual mapping setup

```c
early_fixmap_init();
early_ioremap_init();
```

These prepare temporary virtual mappings before the full MM subsystem is ready.

Used for things like:

```text
DTB mapping
early console
MMIO access
EFI tables
```

---

## 4. Device Tree parsing

```c
setup_machine_fdt(__fdt_pointer);
```

ARM64 normally receives the DTB physical address from the bootloader in register `x0`.

Flow:

```text
Bootloader
   |
   | x0 = physical address of DTB
   v
head.S saves it
   |
   v
__fdt_pointer
   |
   v
setup_machine_fdt()
   |
   +--> fixmap_remap_fdt()
   +--> early_init_dt_scan()
   +--> memblock_reserve()
   +--> of_flat_dt_get_machine_name()
```

If the DTB is invalid, ARM64 prints a critical error and stops very early. The source also notes that the DTB must be 8-byte aligned and not exceed 2 MB. ([GitHub][1])

---

## 5. Early parameter parsing

```c
parse_early_param();
```

Parses early boot arguments such as:

```text
earlycon
mem=
nokaslr
initrd=
console=
```

These must be parsed before normal kernel parameter handling.

---

## 6. Exception mask state

```c
local_daif_restore(DAIF_PROCCTX_NOIRQ);
```

ARM64 uses `DAIF` bits for exception masking:

```text
D = Debug
A = SError
I = IRQ
F = FIQ
```

At this stage, Debug and SError may be unmasked, but IRQ/FIQ remain disabled until interrupt controller setup is ready. ([GitHub][1])

---

## 7. Memory discovery and reservation

```c
arm64_memblock_init();
```

This initializes early physical memory management using `memblock`.

Important structures:

```c
memblock.memory
memblock.reserved
```

Conceptually:

```text
RAM from DT/EFI
    |
    v
memblock.memory

Kernel image, DTB, initrd, reserved regions
    |
    v
memblock.reserved
```

---

## 8. Page table setup

```c
paging_init();
```

This creates the kernel page tables.

Flow:

```text
Physical RAM
    |
    v
memblock regions
    |
    v
kernel linear mapping
    |
    v
page tables
```

After this, the kernel has a proper virtual memory layout.

---

## 9. ACPI or Device Tree path

```c
acpi_boot_table_init();

if (acpi_disabled)
    unflatten_device_tree();
```

ARM64 supports both:

```text
Device Tree boot
ACPI boot
```

Device Tree path:

```text
flat DTB
   |
   v
unflatten_device_tree()
   |
   v
struct device_node tree
```

ACPI path:

```text
EFI / ACPI tables
   |
   v
ACPI parser
   |
   v
platform description
```

---

## 10. Boot memory setup

```c
bootmem_init();
```

Initializes zones and early memory layout for the page allocator.

Conceptually:

```text
memblock
   |
   v
zones
   |
   v
page allocator structures
```

---

## 11. KASAN setup

```c
kasan_init();
```

If enabled, initializes Kernel Address Sanitizer shadow memory.

---

## 12. Standard resources

```c
request_standard_resources();
```

Registers kernel memory regions into `/proc/iomem`.

Example:

```text
System RAM
Kernel code
Kernel data
reserved
```

The ARM64 source defines standard resources for kernel code and kernel data. ([GitHub][1])

---

## 13. PSCI setup

```c
psci_dt_init();
```

or:

```c
psci_acpi_init();
```

PSCI = Power State Coordination Interface.

Used for:

```text
CPU bring-up
CPU shutdown
CPU suspend
system reset
```

---

## 14. CPU operations and SMP discovery

```c
init_bootcpu_ops();
smp_init_cpus();
smp_build_mpidr_hash();
```

ARM64 CPUs are identified using `MPIDR_EL1`.

Flow:

```text
Device Tree / ACPI CPU nodes
    |
    v
logical CPU IDs
    |
    v
MPIDR hardware IDs
    |
    v
cpu_logical_map[]
    |
    v
mpidr_hash
```

The source has `__cpu_logical_map[NR_CPUS]` and helper `cpu_logical_map()` for mapping Linux logical CPU IDs to hardware CPU IDs. ([GitHub][1])

---

# Compact Cheat Sheet

```text
setup_arch(&command_line)
```

Purpose:

```text
Prepare ARM64-specific boot environment.
```

Main responsibilities:

```text
1. Set kernel memory boundaries
2. Connect boot command line
3. Initialize KASLR
4. Prepare early mappings
5. Parse Device Tree / ACPI
6. Initialize memblock
7. Build kernel page tables
8. Initialize boot memory
9. Register system RAM resources
10. Initialize PSCI
11. Discover CPUs
12. Build MPIDR CPU mapping
```

---

# Mental Model

```text
Bootloader gives:
    DTB pointer
    kernel image location
    optional initrd
    command line

setup_arch() converts this into:
    parsed hardware description
    memory map
    page tables
    CPU topology
    firmware interface
    final command_line
```

Final one-line summary:

```text
setup_arch(&command_line) is the ARM64 early boot function that transforms bootloader-provided raw information into kernel-ready architecture state.
```

