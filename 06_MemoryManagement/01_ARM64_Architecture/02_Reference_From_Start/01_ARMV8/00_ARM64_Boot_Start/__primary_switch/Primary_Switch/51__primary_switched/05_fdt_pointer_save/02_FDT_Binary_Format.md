# Flattened Device Tree — Binary Format and Memory Layout

## FDT Magic and Header

The DTB starts with a 48-byte header:

```c
// include/linux/of_fdt.h / devicetree.org spec
struct fdt_header {
    __be32 magic;                 // 0xd00dfeed — big-endian magic
    __be32 totalsize;             // total size of the blob in bytes
    __be32 off_dt_struct;         // offset to structure block (node tree)
    __be32 off_dt_strings;        // offset to strings block
    __be32 off_mem_rsvmap;        // offset to memory reservation block
    __be32 version;               // version (17 for DTB format v17)
    __be32 last_comp_version;     // lowest version compatible (16)
    __be32 boot_cpuid_phys;       // physical CPU ID of boot CPU
    __be32 size_dt_strings;       // size of strings block
    __be32 size_dt_struct;        // size of structure block
};
```

The magic `0xd00dfeed` is stored **big-endian** even on little-endian systems.
The kernel checks: `fdt32_to_cpu(hdr->magic) == FDT_MAGIC`

---

## FDT Memory Layout

```
FDT blob at __fdt_pointer:
┌────────────────────────────────────────────────────────────┐ ← __fdt_pointer
│  fdt_header (48 bytes)                                     │
│  magic = 0xd00dfeed                                        │
│  totalsize = N                                             │
│  off_dt_struct = S                                         │
│  off_dt_strings = T                                        │
│  ...                                                       │
├────────────────────────────────────────────────────────────┤
│  Memory Reservation Block                                   │
│  (list of {address, size} pairs to NOT use for memory)     │
│  Terminated by {0, 0}                                      │
├────────────────────────────────────────────────────────────┤ ← base + off_dt_struct
│  Structure Block                                           │
│  FDT_BEGIN_NODE token (u32 = 1)                            │
│  Node name (null-terminated string)                        │
│  FDT_PROP token (u32 = 3)                                  │
│  Property length, name offset, property data               │
│  ...nested nodes...                                        │
│  FDT_END_NODE token (u32 = 2)                              │
│  FDT_END token (u32 = 9)                                   │
├────────────────────────────────────────────────────────────┤ ← base + off_dt_strings
│  Strings Block                                             │
│  (null-terminated property name strings)                   │
│  "compatible\0device_type\0reg\0interrupts\0..."           │
└────────────────────────────────────────────────────────────┘
```

---

## Example FDT Content (ARM64 Raspberry Pi style)

```dts
// Abbreviated DTS (source format):
/ {
    compatible = "raspberrypi,4-model-b", "brcm,bcm2711";
    #address-cells = <2>;
    #size-cells = <1>;

    memory@0 {
        device_type = "memory";
        reg = <0x0 0x00000000 0x40000000>;  // 1GB at PA 0
    };

    chosen {
        bootargs = "console=ttyS0,115200 root=/dev/mmcblk0p2";
        linux,initrd-start = <0x0 0x10000000>;
        linux,initrd-end   = <0x0 0x10800000>;
    };

    cpus {
        #address-cells = <1>;
        #size-cells = <0>;
        cpu@0 {
            compatible = "arm,cortex-a72";
            reg = <0>;
            enable-method = "spin-table";
            cpu-release-addr = <0x0 0x000000e0>;
        };
    };
};
```

---

## How the Kernel Validates and Scans the FDT

```c
// arch/arm64/kernel/setup.c
void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    void *dt_virt = fixmap_remap_fdt(dt_phys, &dt_size, PAGE_KERNEL_RO);
    
    if (!dt_virt || !early_init_dt_scan(dt_virt)) {
        // No valid DT — try to use ACPI or panic
        early_init_dt_verify(dt_virt);
        ...
    }
    
    unflatten_device_tree();    // convert flat DTB → OF node tree
}
```

`early_init_dt_scan` scans three critical nodes:
1. `/` root: gets `#address-cells`, `#size-cells`
2. `/chosen`: gets `bootargs`, `initrd` range
3. `/memory`: calls `early_init_dt_add_memory_arch()` for each region → `memblock_add()`

---

## FDT Lifetime in the Kernel

```
Boot:         x21 = FDT physical address (from bootloader via x0)
After MMU on: x21 = FDT virtual address (linear map)
              str_l x21, __fdt_pointer  ← our instruction saves it

early_init:   fixmap_remap_fdt(__fdt_pointer)  → maps FDT read-only
              early_init_dt_scan()             → extracts critical info

post-init:    unflatten_device_tree()           → builds of_node tree
              fdt parsed into struct device_node* tree

Later:        __fdt_pointer: freed (it's __initdata)
              The FDT blob itself: NOT freed (reserved in memblock)
              device_node tree: used throughout kernel lifetime
```

The `__fdt_pointer` variable is `__initdata` — it can be freed after the boot
process is complete (after `free_initmem()`). But the FDT blob memory itself is
marked reserved in the memory allocator and is NOT freed.

---

## ACPI vs FDT on ARM64

Modern ARM64 server hardware may use ACPI instead of (or in addition to) FDT:
- ACPI: primarily for server/UEFI-based systems (SBSA spec)
- FDT: for embedded systems, Raspberry Pi, most mobile/automotive

The kernel selects DT or ACPI based on whether FDT is present and valid. If
`x0 = 0` at entry (no FDT), the kernel falls back to ACPI/UEFI firmware tables.
The `__fdt_pointer` save in `__primary_switched` is thus CONDITIONAL on FDT being
available — if `x21 = 0`, `__fdt_pointer` is stored as 0 and ACPI takes over.

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