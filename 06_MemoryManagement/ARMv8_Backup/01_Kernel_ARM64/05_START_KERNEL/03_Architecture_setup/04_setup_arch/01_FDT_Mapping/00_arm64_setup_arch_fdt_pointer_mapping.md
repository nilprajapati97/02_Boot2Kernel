For **ARMv8 / ARM64**, this exact line is **not the normal ARM64 flow**:

```c
if (__atags_pointer)
    atags_vaddr = FDT_VIRT_BASE(__atags_pointer);
```

That line is from the **ARM32** boot path. ARM64 does **not** use legacy ATAGS or `__machine_arch_type`; it normally receives a **DTB physical pointer in register `x0`**.

ARM64 equivalent idea is:

```text
Bootloader
   |
   | x0 = physical address of DTB
   v
early head.S code
   |
   v
__fdt_pointer
   |
   v
setup_arch()
   |
   v
setup_machine_fdt(__fdt_pointer)
```

So conceptually, ARM64 does this:

```c
setup_machine_fdt(__fdt_pointer);
```

not:

```c
FDT_VIRT_BASE(__atags_pointer);
```

### ARMv8 meaning

On ARMv8, the bootloader passes the **Flattened Device Tree Blob** address in `x0`.

```text
x0 = DTB physical address
```

The kernel saves this into an internal variable such as:

```c
__fdt_pointer
```

Then during `setup_arch()`, ARM64 maps that physical DTB address using early mapping/fixmap logic so the kernel can safely read it before the full memory manager is ready.

### Why mapping is needed

The DTB address from bootloader is a **physical address**.

But C code in the kernel usually works with **virtual addresses**.

So ARM64 must temporarily map:

```text
DTB physical address
        |
        v
temporary kernel virtual address
        |
        v
early_init_dt_scan()
```

### ARM64 flow

```text
Bootloader places DTB in RAM
        |
        v
x0 contains DTB physical address
        |
        v
Kernel entry saves x0 as __fdt_pointer
        |
        v
setup_arch()
        |
        v
early_fixmap_init()
        |
        v
early_ioremap_init()
        |
        v
setup_machine_fdt(__fdt_pointer)
        |
        v
map DTB temporarily
        |
        v
early_init_dt_scan()
        |
        v
extract memory, command line, initrd, CPU info
```

### Simple comparison

```text
ARM32:
    r2 -> __atags_pointer -> ATAGS or DTB

ARM64:
    x0 -> __fdt_pointer   -> DTB only
```

### Final ARMv8 interpretation

For ARMv8, the equivalent concept is:

> “If the bootloader passed a DTB physical address in `x0`, the kernel stores it as `__fdt_pointer`, maps it temporarily into virtual memory, and parses it during `setup_machine_fdt()`.”
