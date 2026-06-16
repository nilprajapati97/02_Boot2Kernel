# x21 — FDT Pointer Complete Lifecycle

**Register:** `x21`  
**Value:** Physical address of the Flat Device Tree (FDT) blob  
**Passed by:** Bootloader → kernel entry (`x0` at primary_entry) → saved to `x21`

---

## 0. What Is the FDT and Why Does It Matter?

The **Flat Device Tree** (FDT, also called Device Tree Blob, DTB) is a binary
data structure that describes the hardware platform to the OS. On ARM64 systems,
this is the primary mechanism for the bootloader to describe:

- CPU topology (number of CPUs, CPU types, affinities)
- Memory regions (RAM ranges, reserved memory)
- Peripherals (GIC interrupt controller, UART, clock controllers, PCIe, etc.)
- Power management (PSCI methods for CPU on/off)
- Platform quirks and compatibility strings

Without the FDT, the kernel cannot initialize any hardware. If `x21` is wrong
(null, wrong PA, not a valid DTB), the kernel will fail in `setup_arch()`.

---

## 1. ARM64 Kernel Boot Protocol: Register Conventions

From `Documentation/arm64/booting.rst`:

```
At primary_entry, the bootloader must:
    x0 = physical address of device tree blob (FDT)
    x1 = 0 (reserved — must be 0)
    x2 = 0 (reserved — must be 0)
    x3 = 0 (reserved — must be 0)
```

All other registers are "don't care" from the bootloader's perspective.

**At `primary_entry`**, the kernel immediately saves `x0` → `x21`:

```asm
// arch/arm64/kernel/head.S
SYM_CODE_START(primary_entry)
    ...
    mov     x21, x0             // x21 = PA of FDT blob
    ...
```

---

## 2. The Complete x21 Journey

### Phase 1: Bootloader → primary_entry

```
Bootloader (e.g., U-Boot, GRUB, EDK2):
1. Loads kernel binary to PA (e.g., 0x4000_0000)
2. Loads FDT to PA (e.g., 0x8000_0000)
3. Jumps to kernel primary_entry with:
   x0 = 0x8000_0000 (FDT PA)
   x1-x3 = 0
```

### Phase 2: primary_entry

```asm
mov     x21, x0             // x21 = FDT PA (save before x0 is clobbered)
bl      __cpu_setup         // x0 clobbered by return value (SCTLR_EL1 value)
                            // x21 preserved (callee-saved)
```

### Phase 3: __primary_switch

```asm
// __primary_switch receives x21 from caller (primary_entry)
// x21 = FDT PA

bl      __pi_early_map_kernel(x0=x21, ...)  // x21 passed in x0 to C function
                                             // Wait — is it passed as x0?
```

Actually in `__primary_switch`:

```asm
SYM_FUNC_START_LOCAL(__primary_switch)
    ...
    mov     x0, x21         // FDT PA → argument to __pi_early_map_kernel
    bl      __pi_early_map_kernel
    ...
    // after return, x21 still holds FDT PA (saved by callee)
```

### Phase 4: __pi_early_map_kernel

The FDT PA is passed to `__pi_early_map_kernel` to:
1. Read FDT memory reservations (ensure kernel doesn't map over reserved regions)
2. Map the FDT itself into the kernel VA space (so `setup_arch` can read it)

```c
// arch/arm64/mm/pi/map_kernel.c
asmlinkage void __init __pi_early_map_kernel(unsigned long fdt_pa, ...)
{
    ...
    /* Map the FDT early so we can read it */
    map_fdt(fdt_pa);
    ...
}
```

### Phase 5: __primary_switched

```asm
// arch/arm64/kernel/head.S — __primary_switched:
...
mov     x0, x21             // FDT PA → argument to __fdt_pointer setup
adr_l   x4, __fdt_pointer   // Address of global variable
str     x21, [x4]           // Store FDT PA to __fdt_pointer
```

`__fdt_pointer` is a global variable that holds the FDT physical address for
later use by the C code.

### Phase 6: start_kernel → setup_arch → unflatten_device_tree

```c
// init/main.c
void start_kernel(void)
{
    ...
    setup_arch(&command_line);  // Uses __fdt_pointer
    ...
}

// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    ...
    unflatten_device_tree();    // Parse FDT, create device_node tree
    ...
}
```

`unflatten_device_tree()` reads the FDT binary and creates the
`of_node` tree used by all driver probe code.

---

## 3. FDT Validation in Early Boot

Before the FDT is used, the kernel verifies it:

```c
// arch/arm64/kernel/setup.c
early_init_dt_scan(phys_to_virt(__fdt_pointer));
    → fdt_check_header(fdt)     // Magic number = 0xD00DFEED
    → fdt_totalsize(fdt)        // Bounds check
    → early_init_dt_scan_memory() // Parse /memory nodes
```

**FDT magic number:** `0xD00DFEED` (big-endian "dood feed" — a classic embedded
systems in-joke). If the FDT PA is wrong (e.g., the bootloader set `x0` to a
garbage value), the magic number check fails and the kernel prints:

```
Error: invalid dtb and unrecognized/unsupported machine ID
```

---

## 4. FDT Physical Address in the Memory Map

The FDT blob is placed in memory by the bootloader. ARM64 boot protocol
requirements for FDT placement:

```
1. Must be placed in RAM (must be accessible to the kernel)
2. Must NOT overlap with the kernel binary
3. Must NOT overlap with the initrd (if any)
4. Must be 8-byte aligned (FDT header field alignment requirement)
5. Must be within the first 512 MB of RAM (relaxed in modern kernels)
```

Typical bootloader placement:

```
Physical Memory Map (example):
0x4000_0000  Kernel binary (primary_entry code starts here)
0x4A00_0000  End of kernel binary
0x4A00_0000  Initrd (if any)
0x8000_0000  Device Tree Blob (FDT)
0x8001_0000  End of FDT (typical size: 32KB–128KB)
```

---

## 5. `__fdt_pointer` — The Persistent Reference

```c
// arch/arm64/kernel/setup.c
u64 __fdt_pointer __initdata;
```

`__initdata` means this variable is in the `.init.data` section, which is
freed after kernel init:

```
sys_call(free_initmem) → free pages in [__init_begin, __init_end]
```

After `free_initmem`, `__fdt_pointer` is gone. But by that time, `unflatten_device_tree`
has already parsed the FDT and created the persistent `device_node` tree in
normal kernel memory. The FDT binary itself is no longer needed.

---

## 6. FDT Address Space Transitions

The FDT starts as a **physical address** (in x21). It is later accessed as a
**virtual address**:

```c
// Physical to virtual translation:
fdt_vaddr = phys_to_virt(__fdt_pointer);
// = PA + PAGE_OFFSET
// = PA + (-(1UL << VA_BITS))
// = PA + 0xFFFF_0000_0000_0000 (for 48-bit VA)
```

This translation is valid only after `__pi_early_map_kernel` maps the FDT into
the kernel VA space. The mapping ensures that `phys_to_virt(fdt_pa)` gives a
valid VA for reading the FDT.

---

## 7. Why x21 is the Correct Register Choice

Registers `x19`–`x28` are callee-saved. Within this set:
- `x19` is typically used for the first important callee-saved register in a function
- `x20` is used for boot mode
- `x21` is used for FDT pointer

These register assignments are not mandated by the ABI — they are
**kernel convention**. The choice of `x20` and `x21` appears in comments in
`head.S`:

```asm
/*
 * x20 = cpu boot mode
 * x21 = FDT pointer (physical)
 */
```

This explicit comment documents the kernel's register usage convention for
the early boot code, which is not enforced by the compiler but maintained by
human discipline.

---

## 8. FDT Lifecycle Summary

| Phase | FDT Location | Access Mode | Who Uses It |
|---|---|---|---|
| Bootloader sets it | Physical RAM | Physical address in x0 | Bootloader |
| `primary_entry` | Physical RAM | x21 = PA | Kernel head.S |
| `__pi_early_map_kernel` | Physical RAM | C pointer (via x0) | Page table builder |
| `__primary_switched` | Physical RAM | x21 stored to `__fdt_pointer` | head.S |
| `setup_arch` | Virtual (PA+PAGE_OFFSET) | `phys_to_virt(__fdt_pointer)` | C kernel code |
| `unflatten_device_tree` | Virtual | `of_fdt_unflatten_tree` | DT parser |
| After `free_initmem` | N/A | `__fdt_pointer` freed | N/A (device_node tree persists) |

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