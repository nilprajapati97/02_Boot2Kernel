# Save FDT Pointer Globally — `__fdt_pointer`, Boot Register Retirement, and DT Lifecycle

**File**: `arch/arm64/kernel/head.S` — inside `__primary_switched`
**Instructions**:
```asm
str_l   x21, __fdt_pointer, x5    // Store FDT phys addr into __fdt_pointer global
```
**Perspective**: Memory Architecture / Device Tree Boot Protocol
**Style**: Google Kernel Documentation / NVIDIA Platform Boot Guide

---

## 1. The FDT Journey: From Bootloader to `__fdt_pointer`

The FDT (Flattened Device Tree) physical address has been carried in a
**callee-saved register** since the very first instruction of the kernel:

```
Bootloader:          x0 = FDT physical address  (ARM64 boot protocol)
                         ↓
primary_entry:       x21 = x0   (preserve_boot_args: mov x21, x0)
                         ↓
[through all boot:   x21 preserved across all bl calls because
 record_mmu_state,   x21 is callee-saved. Any called function
 init_kernel_el,     saves and restores it if it uses it.]
 __cpu_setup,
 __primary_switch,
 __pi_early_map_kernel, ← x21 passed as arg (fdt param)
 __primary_switched]     ↓
                     str_l x21, __fdt_pointer    ← THIS STEP
```

After this store, the FDT physical address is safely in a global variable.
`x21` can be overwritten by subsequent `bl` calls without losing the FDT.

---

## 2. `str_l` Macro: Store to a Far Symbol

```asm
str_l   x21, __fdt_pointer, x5

// Expands to:
adrp    x5, __fdt_pointer          // x5 = page-aligned address of __fdt_pointer
str     x21, [x5, :lo12:__fdt_pointer]  // store x21 to full address
```

`adrp` computes a PC-relative address valid within ±4GB of the current PC.
Since we are now executing at a virtual address and `__fdt_pointer` is in
the kernel data section (also at virtual address), this PC-relative addressing
works correctly.

`x5` is used as the **scratch register** for the address computation. It is
a caller-saved register (temporary), so using it here does not violate any
ABI contract.

---

## 3. `__fdt_pointer` — The Global Variable

```c
// arch/arm64/kernel/setup.c
phys_addr_t __fdt_pointer __initdata;
```

**Key attributes**:
- `phys_addr_t` — stores a **physical** address (not virtual)
- `__initdata` — lives in `.init.data`, freed after boot

The downstream consumer:

```c
// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    setup_machine_fdt(__fdt_pointer);  // ← reads this global
    ...
}
```

`setup_machine_fdt` → `fixmap_remap_fdt` → `early_init_dt_scan` →
`unflatten_device_tree` — this entire chain uses `__fdt_pointer` as the
starting point for all device tree parsing.

---

## 4. Why Physical Address (Not Virtual)?

`__fdt_pointer` stores the **physical address** of the FDT blob, not a
kernel virtual address.

The FDT is placed in RAM by the bootloader at an arbitrary physical address
(typically right after the kernel image, or in a reserved memory region).
At the time `preserve_boot_args` saves x21, the kernel has **not yet mapped
the FDT** into any virtual address range (that happens in `map_fdt` inside
`early_map_kernel`).

`setup_machine_fdt` receives the physical address and immediately uses
`fixmap_remap_fdt()` to create a temporary kernel virtual mapping before
accessing any FDT content:

```c
// arch/arm64/kernel/setup.c
static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    void *dt_virt = fixmap_remap_fdt(dt_phys, &dt_size, PAGE_KERNEL);
    // dt_virt = fixmap virtual address of the FDT
    // all FDT access goes through dt_virt
}
```

The physical address is a stable, absolute identifier for the FDT location
that remains valid regardless of what virtual address mappings exist.

---

## 5. Register Retirement: x21 Is Now Free

```
Register x21 lifecycle:
  preserve_boot_args():   x21 ← x0 (FDT phys addr from bootloader)
  early_map_kernel():     x21 passed as arg (FDT used for KASLR + feature init)
  __primary_switched:     str_l x21, __fdt_pointer  ← RETIREMENT POINT
  After this line:        x21 no longer carries boot-critical state
                          Can be freely used by subsequent C code
```

The three boot registers are retired in order in `__primary_switched`:
```
x21 (FDT):        retired HERE (str_l x21, __fdt_pointer)
x20 (boot mode):  retired in set_cpu_boot_mode_flag + finalise_el2
x19 (MMU state):  fully retired after init_kernel_el (consumed in primary_entry)
```

---

## 6. Device Tree Lifecycle After `__fdt_pointer` Is Written

```
Boot sequence:
  __primary_switched: __fdt_pointer = FDT_PHYS_ADDR       [assembly]
        ↓
  start_kernel → setup_arch → setup_machine_fdt()         [C, fixmap access]
        ↓
  unflatten_device_tree()                                  [build struct device_node tree]
        ↓
  of_platform_populate()                                   [create platform_device for each node]
        ↓
  Free: __fdt_pointer is in __initdata — freed by free_initmem() at boot end
        The unflattened device tree (struct device_node) persists forever.
```

The physical FDT blob itself is reserved in `memblock` by
`early_init_fdt_scan_reserved_mem()` to prevent the memory allocator from
overwriting it before `unflatten_device_tree` completes. After unflattening,
the reservation can be released.

---

## 7. NVIDIA Platform Note: FDT Size and Placement

On NVIDIA Tegra/Orin platforms, the DTB (Device Tree Blob) is typically:
- **Size**: 100KB–400KB (large due to NUMA topology, PCIe, display, GPU entries)
- **Placement**: Loaded by U-Boot/UEFI into RAM, address passed in x0
- **Alignment**: 8-byte aligned (FDT header requirement)

The FDT physical address in x0 (→ x21 → `__fdt_pointer`) is the only
mechanism the kernel has to find the device tree. There is no hardcoded
fallback — if x0 is wrong (bootloader bug), the kernel will:
1. Read garbage as an FDT header
2. Fail the `fdt_check_header()` call in `setup_machine_fdt`
3. Panic with: `"Booting Linux without a device tree"`
