# `__fdt_pointer` — From Boot to `unflatten_device_tree`

## The Full Pipeline: Boot Arg → Global Var → OF Tree

```
[Bootloader]
    x0 = FDT physical address
    Jump to primary_entry
         │
         ▼
[preserve_boot_args]
    x21 = x0 = FDT PA
    boot_args[0] = FDT PA (for secondary CPUs / kexec)
         │
         ▼ (many functions called, x21 preserved as callee-saved)
         │
[__primary_switched]
    str_l x21, __fdt_pointer, x5
    __fdt_pointer = FDT PA
         │
         ▼
[start_kernel → setup_arch]
    setup_machine_fdt(__fdt_pointer)
         │
         ├─► fixmap_remap_fdt(dt_phys) → maps FDT to fixmap VA
         │
         ├─► early_init_dt_scan(dt_virt):
         │       early_init_dt_scan_root()    → get #address-cells, #size-cells
         │       early_init_dt_scan_chosen()  → extract /chosen/bootargs, initrd
         │       early_init_dt_scan_memory()  → call memblock_add() for each mem region
         │
         └─► unflatten_device_tree():
                 start_kernel → rest_init → kernel_init → do_initcalls
                 OR:
                 setup_arch → unflatten_device_tree()
                     │
                     └─► of_node = build_of_node_tree(fdt)  // alloc struct device_node*
```

---

## `early_init_dt_scan` — Extracting Critical Early Data

### Memory Discovery
```c
// drivers/of/fdt.c
int __init early_init_dt_scan_memory(unsigned long node, const char *uname, ...)
{
    const __be32 *reg = of_get_flat_dt_prop(node, "reg", &l);
    ...
    while (l >= (dt_root_addr_cells + dt_root_size_cells) * sizeof(__be32)) {
        base = dt_mem_next_cell(dt_root_addr_cells, &reg);
        size = dt_mem_next_cell(dt_root_size_cells, &reg);
        early_init_dt_add_memory_arch(base, size);  // → memblock_add()
    }
}
```

Without this scan, `memblock` has no knowledge of available RAM → `kmalloc`
cannot work → entire kernel initialization fails.

### Command Line Extraction
```c
// drivers/of/fdt.c
int __init early_init_dt_scan_chosen(char *cmdline)
{
    p = of_get_flat_dt_prop(node, "bootargs", &l);
    if (p != NULL && l > 0) {
        strlcpy(cmdline, p, min(l, COMMAND_LINE_SIZE));
    }
}
```

This fills `boot_command_line[]` which `start_kernel` then parses via `parse_early_param`.

---

## `unflatten_device_tree` — Building the OF Node Tree

```c
// drivers/of/fdt.c
void __init unflatten_device_tree(void)
{
    __unflatten_device_tree(initial_boot_params, NULL, &of_root,
                            early_init_dt_alloc_memory_arch, false);
    ...
    of_alias_scan(early_init_dt_alloc_memory_arch);
}
```

This traverses the flat DTB and builds a tree of `struct device_node` objects:
```c
struct device_node {
    const char *name;           // node name (e.g., "memory")
    const char *type;           // device_type property
    phandle phandle;            // unique node reference
    const char *full_name;      // full path (e.g., "/cpus/cpu@0")
    struct fwnode_handle fwnode;
    struct property *properties;  // linked list of all properties
    struct device_node *parent;   // parent node
    struct device_node *child;    // first child
    struct device_node *sibling;  // next sibling
    ...
};
```

After `unflatten_device_tree`:
- `of_root` points to the root node `/`
- `of_find_node_by_path("/cpus/cpu@0")` works
- All device drivers can use `of_get_property()`, `of_get_child_by_name()`, etc.

---

## Relationship to `initial_boot_params`

```c
// drivers/of/fdt.c
void *initial_boot_params __ro_after_init;

// arch/arm64/kernel/setup.c
void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    void *dt_virt = fixmap_remap_fdt(dt_phys, &dt_size, PAGE_KERNEL_RO);
    ...
    initial_boot_params = dt_virt;   // save VA of FDT
    ...
}
```

`initial_boot_params` is the virtual address version of `__fdt_pointer`.
`__fdt_pointer` stores PA (raw from boot), `initial_boot_params` stores VA (after mapping).

---

## What Happens if FDT Is Absent or Corrupt?

```c
// arch/arm64/kernel/setup.c
if (acpi_disabled) {
    void *fdt = early_init_dt_scan_nodes();
    if (!fdt) {
        pr_crit("\nError: invalid device tree blob at physical address %pa "
                "(virtual address 0x%px)\n"
                "The dtb must be 8-byte aligned and must not exceed 2 MB in size\n",
                &dt_phys, dt_virt);
        while (true)
            cpu_relax();   // hang — no recovery possible
    }
}
```

Without a valid FDT (and without ACPI tables as alternative), the kernel cannot
discover memory or peripherals and hangs forever in a busy loop. This is why the
`str_l x21, __fdt_pointer` in `__primary_switched` is critical — losing `x21`
before this instruction means unrecoverable boot failure.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The FDT (Flattened Device Tree) is a data structure passed to the kernel by the bootloader in register x1 (per the ARM64 boot protocol). The CPU sees it as a region of Normal memory containing a binary blob in big-endian format. The FDT pointer (PA) is preserved in register x21 throughout __primary_switch and is converted to a VA after the MMU is enabled. The CPU has no FDT-specific hardware support; it is entirely a software convention.

### Kernel Perspective (Linux ARM64)
x21 holds the FDT PA (physical address) from the moment it is received from the bootloader until __primary_switched converts it to a VA using __phys_to_virt(). The FDT is used by:
  - fdt_check_header(): verify the DTB magic number.
  - early_init_dt_scan(): scan memory nodes for physical RAM regions.
  - unflatten_device_tree(): build the in-kernel device tree (struct device_node tree).
  - of_platform_populate(): instantiate platform devices from the DT.
The FDT must remain valid (not overwritten) until early_init_dt_scan() has completed. Linux maps the FDT read-only in the fixmap region to protect it.

### Memory Perspective (ARMv8 Memory Model)
The FDT is a contiguous PA range passed by the bootloader. Before the MMU is enabled, the kernel accesses it directly by PA. After MMU enable, the FDT is accessed via its VA (calculated as __phys_to_virt(x21)). The FDT occupies Normal memory (the bootloader typically places it in DRAM). Linux ensures the FDT is within the initial direct-map (PAGE_OFFSET + PA) so it remains accessible after start_kernel. Once early_init_dt_scan() has parsed the FDT, the raw FDT memory can be released (freed back to the memblock allocator).