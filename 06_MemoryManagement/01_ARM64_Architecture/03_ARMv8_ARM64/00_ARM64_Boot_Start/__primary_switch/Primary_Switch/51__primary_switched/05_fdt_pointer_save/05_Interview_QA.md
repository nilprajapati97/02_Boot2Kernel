# FDT Pointer Save — Interview Q&A

---

## Q1: What is in register x21 when `str_l x21, __fdt_pointer, x5` executes?

**A:** `x21` holds the physical address of the Flattened Device Tree (FDT/DTB) blob
as provided by the bootloader. This value was placed in `x0` by the firmware per
the ARM64 boot protocol, saved to `x21` by `preserve_boot_args` (which copies x0
to x21 and stores it in `boot_args[0]`), and preserved through all subsequent boot
stages because `x21` is a callee-saved register (x19–x28 are callee-saved in
AAPCS64). After MMU enablement, this physical address is still the value stored —
the PA is stored, not a virtual address.

---

## Q2: Why is `x21` used to carry the FDT pointer across boot stages?

**A:** Two reasons:
1. `x21` is a callee-saved register (AAPCS64 §6.1.1). Any function that follows
   the ABI will save and restore x19–x28. This means `x21` survives function calls
   like `__cpu_setup`, `__enable_mmu`, etc. without explicit protection.
2. `x0`–`x7` are parameter/result registers and would be overwritten by the first
   function call. Using `x21` avoids needing to reload from `boot_args[0]` after
   every function call.

---

## Q3: Does `__fdt_pointer` store a physical or virtual address?

**A:** Physical address. `str_l x21, __fdt_pointer, x5` stores the raw PA from the
bootloader. Subsequent C code (`setup_machine_fdt`) treats it as a `phys_addr_t`
and uses `fixmap_remap_fdt()` or `phys_to_virt()` to obtain a usable virtual
address. This design is safer because it avoids depending on the exact VA mapping
being correct at the assembly level.

---

## Q4: What is the difference between `__fdt_pointer` and `initial_boot_params`?

**A:**
- `__fdt_pointer` (u64, `__initdata`): stores the FDT **physical address**, set by
  `str_l x21, __fdt_pointer` in `__primary_switched`. Raw boot value.
- `initial_boot_params` (void *, `__ro_after_init`): stores the FDT **virtual address**,
  set by `setup_machine_fdt` after mapping the FDT into the fixmap. Used by all FDT
  parsing functions (`of_get_flat_dt_prop`, etc.).

`__fdt_pointer` is the bridge from assembly → C. `initial_boot_params` is the
bridge from early init → the rest of the kernel.

---

## Q5: What is the `str_l` macro and why not use a plain `str` instruction?

**A:** `str_l` is a two-instruction sequence:
```asm
adrp  tmp, symbol      // get page-aligned address of symbol (21-bit PC-relative)
str   src, [tmp, :lo12:symbol]  // store using page + 12-bit offset
```
A plain `str x21, __fdt_pointer` is impossible on ARM64 because:
- ARM64 has no "store absolute address" instruction
- PC-relative loads/stores are limited to ±1 MB range (`ldr` literal pool)
- `__fdt_pointer` could be anywhere in the kernel image (beyond ±1 MB from head.S)
- `adrp` + `:lo12:` covers the full 4 GB range around PC using 2 instructions

---

## Q6: What happens to the FDT memory after boot?

**A:**
- The `__fdt_pointer` variable is `__initdata` — stored in `.init.data` section,
  freed by `free_initmem()` after init. The pointer is gone after init.
- The FDT BLOB MEMORY ITSELF is NOT freed. `memblock_reserve()` marks the FDT
  physical address range as reserved. It remains accessible via the linear map.
- The FDT is used by `unflatten_device_tree()` to build the `struct device_node`
  tree. After that, the OF node tree is used, not the raw DTB.
- However, the raw DTB IS still accessible via `/sys/firmware/fdt` (exposed by
  sysfs for userspace tools like `dtc` and `fdtdump`).

---

## Q7: How does KASLR affect the FDT pointer setup?

**A:** KASLR randomizes the kernel image virtual address, NOT the FDT physical address.
The FDT is placed by the bootloader at a firmware-chosen PA (outside KASLR control).
- `x21` = FDT PA (fixed, determined by firmware)
- Kernel image VA = `PAGE_OFFSET + _text_PA + kaslr_offset`
- FDT VA = `PAGE_OFFSET + fdt_PA` (no KASLR randomization applied to data areas)

`str_l x21, __fdt_pointer, x5` stores the FDT PA correctly regardless of KASLR
because `__fdt_pointer` is a symbol in the kernel image (its VA is fixed relative
to `_text`) and `x21` is a raw PA (firmware-set, fixed). KASLR only affects where
the kernel lands, not where the FDT lands.

---

## Q8: On a system with ACPI (like ARM64 server), does this code still run?

**A:** Yes, but `x21` may be 0 (no DTB). The boot protocol allows `x0 = 0` for
ACPI-only systems. If `x0 = 0`, `x21 = 0` is preserved and `__fdt_pointer = 0`.
`setup_machine_fdt(0)` detects this and skips FDT scanning. `acpi_disabled` is
set to 0 (ACPI enabled), and the kernel proceeds via ACPI tables (MADT, DSDT, etc.)
found via UEFI firmware. The code path is:
```c
if (!acpi_disabled)
    acpi_boot_table_init();  // use ACPI, skip FDT
else
    setup_machine_fdt(__fdt_pointer);  // use FDT
```

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