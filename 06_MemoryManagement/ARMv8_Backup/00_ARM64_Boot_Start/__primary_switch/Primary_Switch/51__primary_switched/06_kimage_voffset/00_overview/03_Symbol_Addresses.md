# Kernel Image Virtual Address Space — Symbol Addresses

## Linker Script and Symbol Addresses

The kernel linker script (`arch/arm64/kernel/vmlinux.lds.S`) assigns virtual
addresses to all sections relative to `_text`:

```lds
/* arch/arm64/kernel/vmlinux.lds.S (simplified) */
SECTIONS {
    . = KIMAGE_VADDR + TEXT_OFFSET;    /* where _text starts in VA space */
    _text = .;

    .text : {
        *(.head.text)                   /* head.S: primary_entry */
        *(.text)
    }

    _etext = .;                         /* end of text section */

    . = ALIGN(SZ_4K);
    __init_begin = .;
    .init.text : { ... }                /* __init functions */
    .init.data : { ... }                /* __initdata variables including __fdt_pointer */
    __init_end = .;

    _data = .;
    .data : { ... }                     /* regular data including kimage_voffset */
    _end = .;

    .bss : { ... }                      /* zero-initialized data */
}
```

`KIMAGE_VADDR` is the compile-time assumed virtual base address. With KASLR, the
actual VA at runtime differs — but `kimage_voffset` corrects for this difference.

---

## Key Kernel Image Symbols and Their Typical Addresses

```
_text         : 0xffff800010000000  (kernel image base, no KASLR)
_etext        : 0xffff800012000000  (end of .text, ~32 MB of code)
__init_begin  : 0xffff800012000000  (start of init sections)
__init_end    : 0xffff800012800000  (end of init sections, freed after boot)
_data         : 0xffff800012800000  (start of .data)
_end          : 0xffff800014000000  (end of kernel image)

init_stack    : within .data, aligned to THREAD_SIZE (16 KB)
init_task     : within .data (struct task_struct for swapper)
kimage_voffset: within .data (set at boot, __ro_after_init)
__fdt_pointer : within .init.data (freed after init)
vectors       : within .text (exception vector table, 2 KB aligned)
swapper_pg_dir: within .data (initial page tables)
```

These VAs are from the linker's perspective. The ACTUAL VAs at runtime depend on
KASLR. The PA of each symbol is `symbol_VA - kimage_voffset`.

---

## How Modules Are Placed Relative to the Kernel Image

```
Kernel image: 0xffff800010000000 - 0xffff800014000000 (64 MB example)
                         ↑ kimage_voffset applies here

Module area:  0xffff000000000000 - 0xffff800000000000 (vmalloc area)
                         ↑ kimage_voffset does NOT apply here
```

Modules are placed within ±128 MB of the kernel text (so relative branches work):
```c
// arch/arm64/kernel/module.c
static void *module_alloc_base = (void *)MODULES_VADDR;
// MODULES_VADDR = _etext - MODULES_VSIZE = near the kernel text
```

Module loading uses `vmalloc_exec()` to allocate in the module area, not the
linear map. `kimage_voffset` is irrelevant for module address translation.

---

## The Image Header — Boot Protocol Fields

The kernel image begins with a 64-byte header (for bootloaders):
```c
// arch/arm64/include/asm/image.h
struct arm64_image_header {
    __le32  code0;          /* executable code (or MAGIC) */
    __le32  code1;          /* executable code */
    __le64  text_offset;    /* Image load offset from start of RAM */
    __le64  image_size;     /* Effective Image size */
    __le64  flags;          /* kernel flags */
    __le64  res2;           /* reserved */
    __le64  res3;           /* reserved */
    __le64  res4;           /* reserved */
    __le32  magic;          /* Magic number, little endian, "ARM\x64" */
    __le32  res5;           /* reserved (used for PE/COFF offset) */
};
```

`text_offset` = preferred physical load offset from start of RAM (not VA).
The bootloader uses this to place the kernel. `kimage_voffset` = VA - this PA.

---

## `__pa_symbol` vs `__pa` — Symbol Address Subtlety

```c
// arch/arm64/include/asm/memory.h
#define __pa_symbol(x)  __pa(RELOC_HIDE((unsigned long)(x), 0))

// RELOC_HIDE prevents compiler from treating symbol addresses as constants
#define RELOC_HIDE(ptr, off) ({     \
    unsigned long __ptr;             \
    __asm__ ("" : "=r"(__ptr) : "0"(ptr)); \
    (typeof(ptr)) (__ptr + (off));   \
})
```

Why `RELOC_HIDE`? With KASLR, the linker assumes a fixed symbol address, but
the ACTUAL runtime address differs. If the compiler treats `&_text` as the
linker-assigned constant, constant folding could pre-compute `__pa(_text)` using
the wrong (linker-time) value.

`RELOC_HIDE` forces the compiler to NOT treat the address as a compile-time
constant — it must use the runtime value (which includes KASLR shift) when
computing `__pa(_text)`. Then `kimage_voffset` subtraction gives the correct PA.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The ARM64 kernel virtual memory map uses the upper half of the 64-bit VA space (addresses with the top T1SZ bits = 1, i.e., 0xFFFF_xxxx_xxxx_xxxx for 48-bit). TTBR1_EL1 translates these addresses. The layout from high to low is:
- 0xFFFF_FFFF_FFFF_FFFF: vmalloc region top
- kernel text/data/bss: mapped by kimage_voffset + PA
- linear map: VA = PAGE_OFFSET + PA (direct-map of all physical RAM)
- vmalloc / vmap area
- PCI I/O space / fixmap
The hardware only cares about TTBR1_EL1 root and TCR_EL1 T1SZ. All the regions above are software conventions; the CPU treats them uniformly via the page tables.

### Kernel Perspective (Linux ARM64)
kimage_voffset = (kernel_VA_start - kernel_PA_start). After KASLR, kimage_voffset is set at boot and used by:
- __phys_to_virt(pa): va = pa - PHYS_OFFSET + PAGE_OFFSET
- __virt_to_phys(va): pa = va - kimage_voffset
The kernel linear map is set up in map_kernel and map_mem (arch/arm64/mm/mmu.c). The kernel text is mapped read-only/execute, the data is read-write/no-execute. After start_kernel, paging_init() rebuilds the definitive page tables.

### Memory Perspective (ARMv8 Memory Model)
The virtual memory map is purely a software abstraction enforced by the page tables. Physically, the linear map means every byte of physical RAM has a corresponding kernel VA: VA = PAGE_OFFSET + PA. This allows the kernel to access any physical address by simple arithmetic. The kimage offset separates the kernel text/data from the linear map to allow different permissions: kernel text is mapped Execute (PXN=0) but not writable; linear map is mapped Read-Write (AP=0b01) but not executable (PXN=1, UXN=1). Both regions use Normal Inner-Shareable Write-Back Cacheable attributes (MT_NORMAL).