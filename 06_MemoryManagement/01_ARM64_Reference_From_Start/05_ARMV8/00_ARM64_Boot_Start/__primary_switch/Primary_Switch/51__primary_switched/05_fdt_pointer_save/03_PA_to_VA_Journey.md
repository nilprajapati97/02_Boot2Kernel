# FDT Pointer — Physical to Virtual Address Journey

## Register `x21` — A Callee-Saved Register Used as Boot Argument

ARM64 AAPCS64 designates x19–x28 as callee-saved registers. Linux boot code uses
`x21` to carry the FDT physical address across multiple function calls during early
boot. Because `x21` is callee-saved, any called function that follows the ABI will
preserve it.

---

## Journey of `x21` Through the Boot Sequence

### Step 1: Firmware Passes FDT PA in x0

```
Firmware entry point:
    x0 = FDT physical address (e.g., 0x48000000)
    x1 = 0
    x2 = 0
    x3 = 0
    PC → primary_entry
```

ARM64 Linux Boot Protocol (v0.2, linux/Documentation/arch/arm64/booting.rst):
> "At kernel entry the following must be observed:
> x0 = physical address of device tree blob (dtb) in system RAM."

### Step 2: `primary_entry` Saves to `boot_args` and `x21`

```asm
// arch/arm64/kernel/head.S primary_entry:
SYM_CODE_START(primary_entry)
    bl  preserve_boot_args       // saves x0-x3 to boot_args[0-3]
    ...
    // After preserve_boot_args, x21 holds FDT PA
    // (set explicitly: adrp x21, __fdt_pointer or from boot_args[0])
```

`preserve_boot_args`:
```asm
preserve_boot_args:
    mov     x21, x0             // x21 = FDT physical address
    adr_l   x0, boot_args       // x0 = &boot_args
    stp     x21, x1, [x0]      // boot_args[0] = FDT PA, boot_args[1] = 0
    stp     x2, x3, [x0, #16]  // boot_args[2] = 0, boot_args[3] = 0
    dmb     sy
    dc      civac, x0           // clean cache for boot_args
    ...
    ret
```

After `preserve_boot_args`: **x21 = FDT physical address** (callee-saved, will be
preserved by any ABI-compliant functions called from now on).

### Step 3: MMU Enable and KASLR

```asm
primary_entry:
    ...
    bl  __cpu_setup             // SCTLR, page table attributes (preserves x21)
    b   __primary_switch        // jump (not call) to __primary_switch

__primary_switch:
    ...
    bl  __enable_mmu            // enables MMU: x21 PA now maps to VA (preserves x21)
    ...
    b   __primary_switched      // x21 still = FDT PA (now accessible via VA too)
```

After MMU enable: the FDT physical address in x21 is ALSO accessible via the
linear map virtual address. Both addresses refer to the same memory:
- Physical: `0x48000000` (original x21 value — direct PA)
- Virtual: `0xffff800048000000` (linear map, `PAGE_OFFSET + PA`)

### Step 4: `__primary_switched` Converts and Stores

```asm
__primary_switched:
    ...
    // x21 = FDT physical address (preserved from primary_entry)
    // After MMU on, the linear map makes PA directly accessible via VA:
    // VA = __phys_to_virt(x21) = PAGE_OFFSET + x21
    // But x21 itself doesn't change — it's still the PA
    // The kernel stores the PA in __fdt_pointer and converts later
    str_l   x21, __fdt_pointer, x5   // __fdt_pointer = FDT physical address
```

Wait — is it PA or VA stored in `__fdt_pointer`?

**Answer: PHYSICAL address is stored.**

`__fdt_pointer` is `u64` and holds the physical address. When `setup_machine_fdt`
accesses it:
```c
// arch/arm64/kernel/setup.c
void __init setup_machine_fdt(phys_addr_t dt_phys)
{
    void *dt_virt = fixmap_remap_fdt(dt_phys, ...);  // map PA → VA
```

`fixmap_remap_fdt` maps the physical address to the fixmap virtual area for access.

---

## Why Physical Address, Not Virtual?

Because when `__primary_switched` runs:
- The linear map IS active (MMU is on)
- But `__phys_to_virt(x21)` requires knowing `PAGE_OFFSET` and possibly KASLR offsets
- The safest approach is storing the raw PA, letting C code (`setup_machine_fdt`)
  do the proper address translation using the full virtual memory infrastructure

The PA can be safely remapped via `fixmap_remap_fdt` or `phys_to_virt` in C.

---

## Memory Map at Time of `str_l x21, __fdt_pointer`

```
Physical Memory Map:
┌─────────────────────────────────┐
│  0x40000000: Kernel Image       │ ← _text physical address
│  ...                            │
│  0x48000000: FDT blob           │ ← x21 points here
│  ...                            │
│  0x80000000: RAM (1GB example)  │
└─────────────────────────────────┘

Virtual Memory Map (after MMU on):
┌─────────────────────────────────┐
│  0xffff800040000000: Kernel     │ ← linear map of kernel phys
│  ...                            │
│  0xffff800048000000: FDT VA     │ ← accessible via VA too
│  ...                            │
│  0xffff000000000000: vmalloc    │
│  0xffff800000000000: linear map │
└─────────────────────────────────┘
```

`str_l x21, __fdt_pointer, x5` stores `0x48000000` (PA).
`setup_machine_fdt(__fdt_pointer)` later converts PA → VA for FDT access.

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