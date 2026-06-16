# FDT Pointer Save — Overview

## The Assembly Instruction

```asm
// arch/arm64/kernel/head.S __primary_switched:
str_l   x21, __fdt_pointer, x5    // save FDT virtual address to __fdt_pointer
```

This is a macro-expanded store: save the 64-bit FDT virtual address in register
`x21` into the global kernel variable `__fdt_pointer`.

---

## What Is the FDT?

FDT = **Flattened Device Tree**. Also called DTB = Device Tree Binary.

A Device Tree is a data structure (not code) that describes the hardware topology
of a system:
- CPUs: number, type, frequency, cache sizes
- Memory: base addresses, sizes, attributes
- Peripherals: UARTs, I2C buses, PCI hosts, interrupt controllers
- Clocks, power regulators
- Pin multiplexing, GPIO banks

The FDT format is defined by the **Device Tree Specification** (devicetree.org).
It is compiled from `.dts` (Device Tree Source) to `.dtb` (Device Tree Binary) by
`dtc` (Device Tree Compiler).

---

## How FDT Gets to the Kernel

1. **Firmware (U-Boot, EDK2, etc.)** loads the kernel image into memory and places
   the FDT blob at some physical address.

2. **Firmware sets x0 = physical address of FDT** (per ARM64 boot protocol).
   - x0 = FDT PA (physical address of DTB)
   - x1 = 0 (reserved)
   - x2 = 0 (reserved)
   - x3 = 0 (reserved)

3. **Firmware jumps to kernel entry** (`primary_entry`).

4. **`primary_entry` saves x0 into x21**:
   ```asm
   primary_entry:
       bl  preserve_boot_args    // saves x0-x3 to boot_args[0-3]
       ...
       // x21 = boot_args[0] = FDT physical address (preserved in callee-saved reg)
   ```

5. **After MMU enable and page table setup**, the physical address in x21 is
   converted to a virtual address (VA) via the linear map.

6. **`__primary_switched` stores the VA**:
   ```asm
   str_l   x21, __fdt_pointer, x5   // x21 now holds the FDT virtual address
   ```

---

## The `str_l` Macro

`str_l` is a Linux ARM64 assembly macro for "store long" — storing a 64-bit register
to a symbol address, using a scratch register:

```asm
.macro str_l, src, sym, tmp
    adrp    \tmp, \sym            // tmp = page-aligned PC-relative address of sym
    str     \src, [\tmp, :lo12:\sym]  // store src to [sym] using page + offset
.endm
```

Usage: `str_l x21, __fdt_pointer, x5`
- `x5` = scratch register (tmp)
- `x21` = value to store (FDT virtual address)
- `__fdt_pointer` = destination symbol (a u64 global variable)

The two-instruction sequence is needed because a single `str x21, __fdt_pointer`
doesn't exist — ARM64 can only address memory relative to PC with limited range.
`adrp` covers the page, `:lo12:` adds the page offset.

---

## `__fdt_pointer` — The Global Variable

```c
// arch/arm64/kernel/setup.c
u64 __fdt_pointer __initdata;
```

- Type: `u64` — holds 64-bit virtual address
- Section: `__initdata` — lives in `.init.data` (freed after init)
- Scope: Used during early initialization, freed when init is complete

The FDT itself is not freed — it's kept in the kernel's linear map as read-only
memory for the lifetime of the system. `__fdt_pointer` is the pointer to it.

---

## Who Uses `__fdt_pointer` Later?

In `start_kernel` → `setup_arch`:
```c
// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    ...
    void *fdt = phys_to_virt(__fdt_pointer);  // access the FDT
    setup_machine_fdt(fdt);
    ...
}
```

`setup_machine_fdt`:
1. Validates the FDT magic number (`0xd00dfeed`)
2. Reads `/memory` nodes → calls `memblock_add()` for each memory region
3. Reads `/chosen` node → extracts `bootargs` (kernel command line)
4. Reads `/cpus` → counts number of CPUs
5. Calls `of_scan_flat_dt()` for early scanning

Without `str_l x21, __fdt_pointer`, `setup_arch` would have no FDT and the
kernel would be unable to find memory, peripherals, or the command line — instant boot failure.

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