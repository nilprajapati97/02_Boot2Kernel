# `kimage_voffset` — Overview

## The Three Assembly Instructions

```asm
// arch/arm64/kernel/head.S __primary_switched:
adrp    x4, _text               // x4 = virtual address of _text page (kernel base)
sub     x4, x4, x0              // x4 = VA(_text) - PA(_text) = kimage_voffset
str_l   x4, kimage_voffset, x5  // save to global variable
```

These three instructions compute and save the offset between the kernel's virtual
address and its physical address. This offset is called `kimage_voffset`.

---

## What `kimage_voffset` Represents

The kernel image occupies:
- **Physical memory**: starting at some address determined by firmware/KASLR
- **Virtual memory**: mapped by the kernel page tables at a specific virtual address

`kimage_voffset` = VA_of_kernel_start − PA_of_kernel_start

```
Example (no KASLR):
    _text PA = 0x40200000    (physical address of kernel start)
    _text VA = 0xffff800040200000  (virtual address of kernel start, linear map)
    
    kimage_voffset = 0xffff800040200000 - 0x40200000
                   = 0xffff800000000000
                   = PAGE_OFFSET  (on non-KASLR systems)
```

With KASLR:
```
    _text PA = 0x48000000    (firmware placed kernel here)
    _text VA = 0xffff800040000000 + randomization  (KASLR applies)
    
    kimage_voffset = VA - PA  (computed at runtime, not a constant)
```

---

## Why `kimage_voffset` Is Needed

The fundamental problem: **the kernel cannot know its own physical address at
compile time when KASLR is active.**

Before KASLR: the physical load address was fixed (`CONFIG_PHYS_OFFSET`), so
`__pa(addr)` = `addr - PAGE_OFFSET` (a compile-time constant subtraction).

With KASLR: the kernel image can be loaded at any 2 MB-aligned physical address
within the DRAM range. The compiler doesn't know where. So at runtime:

```c
// arch/arm64/include/asm/memory.h
static inline phys_addr_t __virt_to_phys_nodebug(unsigned long x)
{
    phys_addr_t y = x - kimage_voffset;   // runtime subtraction
    return y;
}

#define __pa(x) __virt_to_phys_nodebug((unsigned long)(x))
```

Without `kimage_voffset`, `__pa()` would give wrong answers → any code that
converts kernel virtual addresses to physical addresses would be wrong →
DMA operations, page table setup, memory allocation would all be broken.

---

## How `x0` Gets There — Input to the Computation

In `__primary_switched`, `x0` holds the physical address of `_text`:

```asm
// arch/arm64/kernel/head.S:
// Before calling __primary_switched, x0 is set to _text PA:
// (from __cpu_setup / __enable_mmu path)
// x0 = phys_offset = physical address where the kernel was loaded
```

Specifically, during `__primary_switch`:
```asm
__primary_switch:
    adrp    x0, reserved_pg_dir       // or other setup
    ...
    bl      __enable_mmu              // returns, x0 may be set to PA
```

The actual path: `adrp x4, _text` gives the VA, and `x0` was set to the
physical address of the kernel by the pre-MMU boot code that set up page tables.

---

## The `kimage_voffset` Global Variable

```c
// arch/arm64/mm/init.c  (or arch/arm64/kernel/image-vars.h)
s64 kimage_voffset __ro_after_init;
EXPORT_SYMBOL(kimage_voffset);
```

- Type: `s64` (signed 64-bit) — can be negative or positive
- Section: `__ro_after_init` — writable during boot init, read-only after
  `mark_readonly()` is called. Cannot be modified after boot.
- Exported: `EXPORT_SYMBOL` — loadable kernel modules need it for `__pa()`
- Set ONCE: in `__primary_switched`, never changed again

---

## The `__ro_after_init` Security Property

`__ro_after_init` variables are:
1. Placed in a special linker section (`.data..ro_after_init`)
2. Writable at boot time (normal RW memory)
3. Made read-only by `mark_readonly()` called from `start_kernel`

This is a security feature: after boot, an attacker cannot modify `kimage_voffset`
to create incorrect PA→VA mappings. If `kimage_voffset` could be changed, an
attacker could craft fake physical addresses for DMA or page table manipulation.

`mark_readonly()` calls `set_memory_ro()` on the `.data..ro_after_init` section,
making it non-writable. Any subsequent write attempt would cause a page fault.

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