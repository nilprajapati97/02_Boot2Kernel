# Device Tree Parsing — ARM64 / Embedded Systems

## What is a Device Tree?

A Device Tree (DT) is a data structure that describes hardware to the OS. It originated from OpenFirmware (Sun SPARC, PowerPC) and is now the standard for embedded ARM systems (Qualcomm, MediaTek, Broadcom).

```
Device Tree Source (.dts)
        │ dtc (device tree compiler)
        ▼
Device Tree Blob (.dtb) — binary format passed to kernel by bootloader
        │ r2 register (ARM32) or x0 register (ARM64)
        ▼
FDT (Flattened Device Tree) in memory
        │ of_fdt.c / libfdt
        ▼
Live Device Tree (in-memory tree of of_node structs)
```

## FDT (Flattened Device Tree) Format

```c
struct fdt_header {
    uint32_t magic;              // 0xD00DFEED
    uint32_t totalsize;          // total blob size
    uint32_t off_dt_struct;      // offset to structure block
    uint32_t off_dt_strings;     // offset to strings block
    uint32_t off_mem_rsvmap;     // offset to memory reservation map
    uint32_t version;            // FDT spec version
    // ...
};
```

## Memory Node Example (Qualcomm SM8650)

```dts
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    memory@80000000 {
        device_type = "memory";
        reg = <0x0 0x80000000 0x0 0x80000000>;   /* 2GB at 2GB */
    };

    memory@880000000 {
        device_type = "memory";
        reg = <0x8 0x80000000 0x0 0x80000000>;   /* 2GB at 34GB */
    };

    cpus {
        cpu@0 {
            compatible = "arm,cortex-a520";
            reg = <0x0 0x100>;   /* MPIDR = Aff1=1, Aff0=0 */
            device_type = "cpu";
        };
        cpu@200 {
            compatible = "arm,cortex-x4";
            reg = <0x0 0x200>;   /* performance core */
        };
    };
};
```

## DT Parsing in setup_arch (ARM64)

```c
// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    // x0 at kernel entry held DTB pointer
    setup_machine_fdt(__fdt_pointer);
        │
        ├── early_init_dt_scan()        // scan for /chosen (cmdline), /memory
        │       ├── early_init_dt_scan_chosen()   // extract bootargs → cmdline
        │       ├── early_init_dt_scan_root()     // address-cells, size-cells
        │       └── early_init_dt_scan_memory()   // add RAM to memblock
        │
        └── unflatten_device_tree()     // build struct device_node tree
```

## Interview Q&A

### Q1: How does the kernel know where the DTB is located in memory?
**A:** The bootloader (U-Boot on embedded, or EFI stub) places the DTB at a known physical address and passes it to the kernel in a register: ARM64 uses `x0`, ARM32 uses `r2`. The kernel assembly entry code (`arch/arm64/kernel/head.S`) saves this register before calling C code. `setup_machine_fdt(__fdt_pointer)` receives the physical address, early-maps it via `early_fixmap`, reads the FDT header magic (`0xD00DFEED`), and reserves the blob in memblock before anything else uses that memory.

### Q2: What is `of_find_node_by_compatible()` and how is it used by drivers?
**A:** `of_find_node_by_compatible(NULL, "qcom,sm8650-pinctrl")` searches the live DT tree for a node with matching `compatible` string. Platform drivers specify a list of compatible strings in their `of_device_id` table. The kernel's DT probing machinery matches DTB nodes to drivers — this is how the Qualcomm I2C driver knows which I2C controller instance to initialize for which hardware address.

### Q3: How does Device Tree handle interrupt routing differently from ACPI?
**A:** DT uses `interrupt-parent` and `interrupts` properties:
```dts
uart@ff000000 {
    interrupt-parent = <&gic>;
    interrupts = <GIC_SPI 56 IRQ_TYPE_LEVEL_HIGH>;
};
```
The kernel's `irq_domain` infrastructure translates DT interrupt specifiers to Linux IRQ numbers. ACPI uses the `_CRS` (Current Resource Settings) method returning `ACPI_RESOURCE_IRQ` structures. Both ultimately call `irq_create_mapping()` to create a hardware→linux IRQ mapping, but DT is static data while ACPI involves runtime AML interpretation.
