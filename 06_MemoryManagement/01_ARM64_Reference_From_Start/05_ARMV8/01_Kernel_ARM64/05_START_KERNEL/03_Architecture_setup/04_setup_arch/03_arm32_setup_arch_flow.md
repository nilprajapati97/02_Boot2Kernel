This code is **ARM 32-bit setup_arch**, not ARMv8/ARM64.

Reason: it uses:

```c
__atags_pointer
setup_machine_tags()
__machine_arch_type
r1, r2
struct machine_desc
```

These are classic **ARM32 boot flow** concepts. ARM64 normally uses FDT/EFI/ACPI, not ATAGS/machine ID. Your previous ARM64 note is related but different from this ARM32-style flow .

# README name

```text
arm32_setup_arch_flow.md
```

# `setup_arch()` ARM32 Flow

```text
start_kernel()
    |
    v
setup_arch(&command_line)
    |
    +--> Get ATAGS/DTB virtual address
    |
    +--> setup_processor()
    |
    +--> Try Device Tree boot
    |       |
    |       +--> setup_machine_fdt()
    |       +--> reserve DTB memory
    |
    +--> If DT failed, try legacy ATAGS
    |       |
    |       +--> setup_machine_tags()
    |
    +--> If no machine matched
    |       |
    |       +--> early_print()
    |       +--> dump_machine_table()
    |
    +--> Store machine_desc
    |
    +--> setup_initial_init_mm()
    |
    +--> copy boot_command_line
    |
    +--> early_fixmap_init()
    +--> early_ioremap_init()
    |
    +--> parse_early_param()
    |
    +--> early_mm_init()
    |
    +--> setup_dma_zone()
    |
    +--> xen_early_init()
    +--> arm_efi_init()
    |
    +--> adjust_lowmem_bounds()
    |
    +--> arm_memblock_init()
    |
    +--> paging_init()
    |
    +--> kasan_init()
    |
    +--> request_standard_resources()
    |
    +--> register restart handler
    |
    +--> unflatten_device_tree()
    |
    +--> arm_dt_init_cpu_maps()
    +--> psci_dt_init()
    |
    +--> SMP setup
    |       |
    |       +--> smp_set_ops()
    |       +--> smp_init_cpus()
    |       +--> smp_build_mpidr_hash()
    |
    +--> reserve_crashkernel()
    |
    +--> vgacon setup
    |
    +--> mdesc->init_early()
```

# Main Structures

## 1. `machine_desc`

```c
const struct machine_desc *mdesc;
```

This describes the board/platform.

Contains things like:

```text
machine name
reboot mode
restart callback
SMP operations
early init callback
memory layout hints
```

Important assignment:

```c
machine_desc = mdesc;
machine_name = mdesc->name;
```

So after this point, the kernel knows:

```text
Which board am I running on?
Which platform callbacks should I use?
```

---

## 2. ATAGS / DTB pointer

```c
if (__atags_pointer)
    atags_vaddr = FDT_VIRT_BASE(__atags_pointer);
```

Despite the name `__atags_pointer`, this can point to:

```text
legacy ATAGS
or
flattened device tree blob
```

Bootloader usually passes this in ARM32 register `r2`.

---

## 3. Machine detection path

```text
First try:
    setup_machine_fdt()

Fallback:
    setup_machine_tags()
```

Meaning:

```text
Modern boot:
    DTB based

Legacy boot:
    ATAGS + machine ID based
```

If both fail:

```c
early_print("Error: invalid dtb and unrecognized/unsupported machine ID");
dump_machine_table();
```

So boot cannot continue safely.

---

## 4. Command line handling

```c
strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
*cmdline_p = cmd_line;
```

Flow:

```text
bootloader command line
    |
    v
boot_command_line
    |
    v
cmd_line
    |
    v
command_line
```

This preserves `boot_command_line` and gives generic kernel code a usable command line.

---

## 5. Memory initialization

```text
setup_initial_init_mm()
early_fixmap_init()
early_ioremap_init()
early_mm_init()
adjust_lowmem_bounds()
arm_memblock_init()
paging_init()
```

Conceptually:

```text
raw physical RAM info
    |
    v
memblock
    |
    v
lowmem/highmem calculation
    |
    v
page tables
    |
    v
normal kernel virtual memory
```

---

## 6. Device Tree expansion

```c
unflatten_device_tree();
```

Before this:

```text
flat DTB blob
```

After this:

```text
struct device_node tree
```

So drivers and platform code can later query hardware like:

```text
cpus
memory
interrupt controller
UART
timer
PSCI
reserved-memory
```

---

## 7. CPU and SMP setup

```c
arm_dt_init_cpu_maps();
psci_dt_init();
smp_init_cpus();
smp_build_mpidr_hash();
```

Flow:

```text
DT CPU nodes
    |
    v
CPU logical map
    |
    v
PSCI / platform SMP ops
    |
    v
secondary CPU boot support
```

---

# Very Short Summary

```text
setup_arch() on ARM32 identifies the machine, parses DTB/ATAGS,
sets command line, initializes early memory/page tables,
builds CPU maps, configures PSCI/SMP, and prepares platform callbacks.
```

