# `__pa()` and `__va()` — How `kimage_voffset` Is Used

## The Core VA↔PA Conversion Macros

```c
// arch/arm64/include/asm/memory.h

/* VA → PA: subtract kimage_voffset */
static inline phys_addr_t __virt_to_phys_nodebug(unsigned long x)
{
    phys_addr_t y = x - kimage_voffset;
    return y;
}

/* PA → VA: add kimage_voffset */
static inline void *__phys_to_virt(phys_addr_t x)
{
    return (void *)((unsigned long)x + kimage_voffset);
}

#define __pa_symbol(x)  __pa(RELOC_HIDE((unsigned long)(x), 0))
#define __va(x)         ((void *)__phys_to_virt((phys_addr_t)(x)))
#define __pa(x)         __virt_to_phys_nodebug((unsigned long)(x))
```

These are the fundamental memory address conversion primitives. Everything else
in the kernel that converts between physical and virtual addresses eventually
calls one of these.

---

## Where `kimage_voffset` Is Used in the Kernel

### 1. DMA Operations

```c
// drivers/iommu/dma-iommu.c, drivers/base/dma-direct.c
phys_addr_t phys = dma_to_phys(dev, dma_addr);
void *virt = __va(phys);
```

When a DMA device writes to memory at a physical address, the kernel converts
that PA to a VA to access the data. `__va(pa)` uses `kimage_voffset`.

### 2. Memory Map Construction

```c
// arch/arm64/mm/init.c
void __init mem_init(void)
{
    ...
    // Called during memory initialization to set up struct page array
    // All PA↔VA conversions use kimage_voffset
}
```

### 3. Page Table Setup

```c
// arch/arm64/mm/mmu.c
phys_addr_t pgd_phys = __pa_symbol(swapper_pg_dir);
// __pa_symbol uses kimage_voffset to get the physical address of swapper_pg_dir
```

Setting `TTBR1_EL1` requires the physical address of the page table. `__pa_symbol`
converts the kernel symbol address (VA) to PA using `kimage_voffset`.

### 4. Crash/kdump

```c
// arch/arm64/kernel/crash_core.c
vmcoreinfo_append_str("KERNELOFFSET=%lx\n", kaslr_offset());
// kaslr_offset() uses kimage_voffset and PAGE_OFFSET
```

`makedumpfile` and crash-analysis tools use `kimage_voffset` from the core dump
to reconstruct VA→PA mappings.

---

## The `__pa()` vs `__pa_symbol()` Distinction

```c
#define __pa(x)         __virt_to_phys_nodebug((unsigned long)(x))
#define __pa_symbol(x)  __pa(RELOC_HIDE((unsigned long)(x), 0))
```

`__pa_symbol` is used for kernel symbols (`.text`, `.data` symbols defined at
link time). `RELOC_HIDE` prevents the compiler from doing constant folding or
treating the address as a compile-time constant (which would be wrong with KASLR).

`__pa` is used for arbitrary virtual addresses that are in the kernel linear map.

**Security note:** `__pa` only works correctly for addresses in the kernel image
linear map. Calling `__pa` on a userspace address or a vmalloc address gives wrong
results. The kernel has debug modes (CONFIG_DEBUG_VIRTUAL) that detect such misuse.

---

## Module Loading and `kimage_voffset`

Loadable kernel modules are mapped into the vmalloc area, NOT the linear map.
For modules, a different conversion is needed:

```c
// Modules are loaded into vmalloc area: 0xffff000000000000–0xffff800000000000
// They cannot use kimage_voffset-based __pa()
// Instead: module text is accessed via page tables, PA retrieved from page structs
phys_addr_t module_phys = page_to_phys(vmalloc_to_page(module_addr));
```

`kimage_voffset` is specifically for kernel image addresses, not all virtual addresses.

---

## Numerical Example

```
System: ARM64 with 4 GB RAM, PAGE_OFFSET = 0xffff800000000000
Kernel: Loaded by bootloader at PA 0x40200000 (no KASLR)
        Mapped at VA 0xffff800040200000 by page tables

kimage_voffset = VA - PA
               = 0xffff800040200000 - 0x40200000
               = 0xffff800000000000

__pa(0xffff800040200000) = 0xffff800040200000 - 0xffff800000000000
                         = 0x40200000  ✓

__va(0x40200000) = 0x40200000 + 0xffff800000000000
                 = 0xffff800040200000  ✓

With KASLR (kernel at PA 0x78000000, random VA offset):
kimage_voffset = 0xffff800028000000 (different value, computed at boot)
__pa(any_kernel_va) still works correctly using this runtime value
```

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